// web/src/main.js
//
// Glue: connect to the mcp-server WS bridge, load the sprite catalog via
// sprite.list, and decode sprites via sprite.decode for Canvas rendering.
//
// Six browse modes; the Catalog / Character / Monster / Combat modes share the
// canvas / palette / direction / playback controls, while Scene and GUI each
// own a dedicated PNG viewport:
//   - Catalog (目录浏览): pick any sprite from the recovered-name tree.
//   - Character (角色查看器): assemble a player sprite by Class -> Action ->
//     Armor/Weapon via the plrgfx filename formula, then decode + play it with
//     8-direction animation.
//   - Scene (场景查看器): scene.list / scene.build complete level renders.
//   - GUI (界面布局查看器, 6th): ui.list / ui.build reconstruct DiabloUI
//     screens from real MPQ art + an approximate-font text layer.
//
// Player CL2 frames are stored direction-major: 8 directions x N frames, so
// frameIndex = dir * framesPerDir + frameInDir (framesPerDir = total/8).

import { McpClient } from "./client.js?v=20261110a";
import { buildTree, renderCatalog, setAllExpanded } from "./catalog.js?v=20261110a";
import { drawFrame, drawComposite, drawScene } from "./viewer.js?v=20261110a";

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

// Effect-overlay layers composited (alpha) onto the current sprite. Each entry:
// { name, frames:[{rgba_base64,width,height}], directions, enabled, dx, dy }.
let overlayLayers = [];
// The decode args used for the current base sprite, so overlays reuse the same
// archive / palette / TRN context.
let lastDecodeArgs = null;

// Skill / combat viewer: stage-ordered full-cast timeline (caster + target +
// effect layers in one scene). spell.list supplies the catalog.
let spellCatalog = null;       // spell.list result
let combatEntry = null;        // currently selected spell / monster attack
let combatStage = 0;           // index into combatEntry.stages
let combatFrame = 0;           // frame index within that stage
let combatCache = new Map();   // "name|width|trn" -> FrameSet
let combatDir = 0;             // scene direction 0..7, caster faces this way
let combatPlayerStandName = "";// for decoding a TRN-tinted player target
let combatActors = {           // FrameSets for the two actors (+ tinted target)
  playerStand: null, playerCast: null, playerTrn: null,
  monsterBase: null, monsterTrn: null,
};
let combatStageFrames = [];    // per stage: [{ frameSet, anchor, role }]
let combatTimeline = [];       // flattened [{ stageIdx, frameIdx }]
let combatPlayIdx = 0;

// A FrameSet keeps frames split per direction so a stage can play entirely
// inside ONE direction instead of silently cycling through all of them.
// { groups: [frames[]...], directions: groups.length }
function makeFrameSet(frames, directions) {
  const n = Math.max(1, directions | 0);
  const fpd = Math.max(1, Math.floor(frames.length / n));
  const groups = [];
  for (let d = 0; d < n; d++) {
    const g = frames.slice(d * fpd, (d + 1) * fpd);
    groups.push(g.length ? g : frames.slice(0, fpd));
  }
  return { groups, directions: n };
}
function frameSetEmpty(fs) {
  return !fs || !fs.groups || !fs.groups.some((g) => g && g.length);
}
// Map a scene direction (0..7) onto however many directions an actor really has.
function combatGroupIndex(directions, sceneDir) {
  const n = Math.max(1, directions | 0);
  if (n === 1) return 0;
  return Math.min(n - 1, Math.round(sceneDir * (n - 1) / 7));
}
function frameSetAt(fs, fi, sceneDir) {
  if (frameSetEmpty(fs)) return null;
  const g = fs.groups[combatGroupIndex(fs.directions, sceneDir)] || fs.groups[0];
  if (!g || !g.length) return null;
  return g[((fi % g.length) + g.length) % g.length];
}
// Direction-independent frame budget for the timeline: the longest direction
// group. frameSetAt wraps fi inside the chosen group, so playback stays in dir.
function frameSetBudget(fs) {
  if (frameSetEmpty(fs)) return 1;
  return Math.max(1, ...fs.groups.map((g) => (g ? g.length : 1)));
}

// 8-direction animation state.
let currentDirections = 1;  // number of directions the current sprite is split into
let framesPerDir = 1;       // frames per direction (total / directions)
let currentDir = 0;         // currently shown direction
let currentFin = 0;         // currently shown frame within the direction
let zoom = 2;               // display magnification (1..8); also scales scene spacing
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

// Scene mode owns the view instead of the shared canvas: two stacked PNG
// layers (base + SOL overlay) inside a pannable/zoomable viewport.
function isSceneMode() {
  const r = document.querySelector('input[name="mode"]:checked');
  return !!r && r.value === "scene";
}

// GUI layout viewer (6th browse mode) also owns the stage: three stacked
// canvases (real MPQ art + frontend text + backend element boxes).
function isGuiMode() {
  const r = document.querySelector('input[name="mode"]:checked');
  return !!r && r.value === "gui";
}

// Modes that replace the shared sprite canvas with their own view; shared
// playback/direction/palette controls are no-ops in these modes.
function isViewOwnedMode() {
  return isSceneMode() || isGuiMode();
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
    await loadSpellList();       // stage-ordered spell / monster-attack catalog
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

// ---- Effect-overlay layers (alpha compositing) ------------------------------

function isMonsterMode() {
  const r = document.querySelector('input[name="mode"]:checked');
  return !!r && r.value === "monster";
}
function isNpcMode() {
  return isMonsterMode() && $("monsterType").value === "npc";
}

// Character skill-effect sprite names for the current class + equipment prefix
// (plrgfx\{class}\{prefix}\{prefix}{type}.cl2, type ∈ lm/fm/qm).
function characterEffectNames() {
  if (!charIndex || !isCharMode()) return [];
  const cp = $("charClass").value;
  const e = charIndex.get(cp);
  if (!e) return [];
  const armor = $("charArmor").value || "l";
  const weapon = $("charWeapon").value || "n";
  const prefix = e.classChar + armor + weapon;
  return [
    { label: "闪电 lm", name: `plrgfx\\${cp}\\${prefix}\\${prefix}lm.cl2` },
    { label: "火 fm", name: `plrgfx\\${cp}\\${prefix}\\${prefix}fm.cl2` },
    { label: "魔法 qm", name: `plrgfx\\${cp}\\${prefix}\\${prefix}qm.cl2` },
  ];
}

// Monster "Special" overlay name: replace the current animation letter with `s`
// (e.g. monsters\zombie\zombien.cl2 -> monsters\zombie\zombies.cl2).
function monsterSpecialNames() {
  if (!monsterMap || !isMonsterMode() || isNpcMode()) return [];
  const cls = $("monsterSel").value;
  const list = monsterMap.get(cls) || [];
  const base = list[0];
  if (!base) return [];
  const name = base.name.replace(/[nwahd]\.cl2$/, "s.cl2");
  if (name === base.name) return [];
  return [{ label: "Special (s)", name }];
}

// Quick-add buttons appropriate to the current browse mode.
function populateOverlayQuick() {
  const el = $("overlayQuick");
  el.innerHTML = "";
  if (isCombatMode()) { el.hidden = true; return; }
  let items = [];
  if (isCharMode()) items = characterEffectNames();
  else if (isMonsterMode()) items = monsterSpecialNames();
  for (const it of items) {
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = it.label;
    b.title = it.name;
    b.addEventListener("click", () => addOverlay(it.name));
    el.appendChild(b);
  }
  el.hidden = items.length === 0;
}

// Generic overlay picker: any sprite from the current catalog can be added.
function populateOverlayPick() {
  const sel = $("overlayPick");
  sel.innerHTML = "";
  const opt0 = document.createElement("option");
  opt0.value = "";
  opt0.textContent = "（选择叠加精灵…）";
  sel.appendChild(opt0);
  for (const s of lastSprites) {
    const o = document.createElement("option");
    o.value = s.name;
    o.textContent = s.name;
    sel.appendChild(o);
  }
}

// Decode an overlay sprite (reusing the base sprite's archive/palette/TRN context)
// and append it to the overlay layer list, then re-render the current frame.
async function addOverlay(name, archiveOverride) {
  if (!name) return;
  if (!lastDecodeArgs) {
    showError("请先选择并解码一个基础精灵，再添加叠加层。");
    return;
  }
  const baseSprite = { name, archive_path: archiveOverride || lastDecodeArgs.archive };
  const args = buildDecodeArgs(baseSprite, name);
  try {
    setMeta("decoding overlay " + name + " …");
    const res = await client.callTool("sprite.decode", args);
    if (!res || !Array.isArray(res.frames) || res.frames.length === 0) {
      showError("叠加精灵解码失败：" + name + " 未返回帧。");
      return;
    }
    overlayLayers.push({
      name,
      frames: res.frames,
      directions: res.directions || 1,
      enabled: true,
      dx: 0,
      dy: 0,
    });
    renderOverlayList();
    redrawCurrent();
  } catch (e) {
    showError("叠加精灵解码失败：" + name + " (" + (e.message || e) + ")");
  }
}

// Render the list of current overlay layers with enable / offset / remove controls.
function renderOverlayList() {
  const el = $("overlayList");
  el.innerHTML = "";
  if (overlayLayers.length === 0) {
    el.innerHTML = '<div class="overlay-empty">（暂无叠加层）</div>';
    return;
  }
  overlayLayers.forEach((ov, i) => {
    const row = document.createElement("div");
    row.className = "overlay-item";

    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = ov.enabled;
    cb.title = "启用/停用此叠加层";
    cb.addEventListener("change", () => { ov.enabled = cb.checked; redrawCurrent(); });

    const nm = document.createElement("span");
    nm.className = "overlay-name";
    nm.textContent = ov.name;
    nm.title = ov.name;

    const dxL = document.createElement("label");
    dxL.className = "ov-off";
    dxL.textContent = "x";
    const dx = document.createElement("input");
    dx.type = "number"; dx.value = ov.dx; dx.size = 3;
    dx.addEventListener("change", () => { ov.dx = Number(dx.value) || 0; redrawCurrent(); });

    const dyL = document.createElement("label");
    dyL.className = "ov-off";
    dyL.textContent = "y";
    const dy = document.createElement("input");
    dy.type = "number"; dy.value = ov.dy; dy.size = 3;
    dy.addEventListener("change", () => { ov.dy = Number(dy.value) || 0; redrawCurrent(); });

    const rm = document.createElement("button");
    rm.type = "button";
    rm.textContent = "✕";
    rm.title = "移除";
    rm.addEventListener("click", () => { overlayLayers.splice(i, 1); renderOverlayList(); redrawCurrent(); });

    row.append(cb, nm, dxL, dx, dyL, dy, rm);
    el.appendChild(row);
  });
}

// ---- Decode + 8-direction playback -----------------------------------------

// Build the sprite.decode argument object for `sprite` (optionally overriding the
// sprite name, used when decoding an effect-overlay variant of the same base).
// Reuses the current palette + TRN settings so overlays match the base sprite.
function buildDecodeArgs(sprite, nameOverride, opts = {}) {
  // recovered.json stores archive paths with a mix of "/" and "\". Normalize
  // them: if the same MPQ reaches the backend under two spellings, StormLib is
  // asked to open it twice and fails ("cannot open archive").
  const norm = (p) => (p || "").trim().replace(/\\/g, "/");
  const archive = norm(sprite.archive_path);
  const palette = $("palette").value;
  const paletteArchive = paletteArchives.get(palette) || "";
  // Monsters/NPCs are spread across MPQs (Hellfire monsters live in
  // hellfire.mpq, base ones in DIABDAT.MPQ, some in hfopt1.mpq, ...), so give
  // sprite.decode the whole known archive stack to search instead of only the
  // single MPQ picked in the monster viewer. Otherwise every Hellfire monster
  // fails with "extract failed (name not found?)" when DIABDAT.MPQ is selected.
  const archives = [];
  const add = (p) => {
    const n = norm(p);
    if (n && !archives.includes(n)) archives.push(n);
  };
  add(archive);
  add(paletteArchive);
  for (const a of allArchives) add(a);
  const args = {
    archive,
    archives,
    name: nameOverride || sprite.name,
    // Numbered missiles\{name}{N}.cl2 effects need an explicit width from
    // spell.list; width:"auto" only works for un-numbered sprite paths.
    width: opts.width !== undefined ? opts.width : "auto",
    palette_name: palette,
  };
  // An explicit TRN (combat debuff tint on the target) wins over the global
  // toggle. Otherwise the toggle uses the class TRN or a chosen preset.
  if (opts.trn) {
    args.apply_trn = true;
    args.trn_name = opts.trn;
  } else if ($("applyTrn").checked) {
    args.apply_trn = true;
    const preset = $("trnPreset").value;
    if (preset === "infra") args.trn_name = "plrgfx\\infra.trn";
    else if (preset === "stone") args.trn_name = "plrgfx\\stone.trn";
    // preset "class" -> let the backend compute plrgfx\trn<class>.trn
  }
  return args;
}

async function onSelectSprite(sprite) {
  lastSprite = sprite;
  hideError();
  $("currentName").textContent = sprite.name;
  $("combatInfo").hidden = true; $("combatInfo").textContent = "";
  // Overlays are tied to the base sprite; clear them whenever a new one is picked.
  overlayLayers = [];
  renderOverlayList();
  // Refresh the overlay pickers for the new context: quick-add buttons depend on
  // the browse mode (character skill effects / monster Special), and the generic
  // picker lists every sprite in the current catalog.
  populateOverlayQuick();
  populateOverlayPick();

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

  setMeta("decoding " + sprite.name + " …");
  stopPlay();
  try {
    const args = buildDecodeArgs(sprite);
    lastDecodeArgs = args;
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
// Composites any enabled effect-overlay layers on top (alpha), aligning each
// overlay's own (dir, fin) so multi-direction sprites stay in sync.
function showFrame(dir, fin) {
  if (isCombatMode() || isViewOwnedMode()) return;   // those modes own the view
  if (!currentFrames) return;
  const idx = dir * framesPerDir + fin;
  const f = currentFrames[idx];
  if (!f) return;
  currentDir = dir;
  currentFin = fin;

  const ovRender = [];
  for (const ov of overlayLayers) {
    if (!ov.enabled || !ov.frames || !ov.frames.length) continue;
    const ovFpd = Math.max(1, Math.floor(ov.frames.length / (ov.directions || 1)));
    let oi = dir * ovFpd + fin;
    if (oi < 0 || oi >= ov.frames.length) oi = ov.frames.length - 1;
    const of = ov.frames[oi];
    if (of) ovRender.push({ rgba: of.rgba_base64, w: of.width, h: of.height, dx: ov.dx, dy: ov.dy, enabled: true });
  }

  drawComposite($("canvas"), f.rgba_base64, f.width, f.height, ovRender, zoom);
  $("dirNo").textContent = String(dir);
  $("frameNo").textContent = `dir ${dir} / ${currentDirections - 1}  · 帧 ${fin} / ${framesPerDir - 1}`;
  updateMeta(idx, dir, fin);
}

function redrawCurrent() {
  showFrame(currentDir, currentFin);
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
  const ovCount = overlayLayers.filter((o) => o.enabled).length;
  if (ovCount > 0)
    s += `  · 叠加层 ${ovCount}`;
  setMeta(s);
}

function startPlay() {
  if (isViewOwnedMode()) return;
  if (isCombatMode()) return startCombatPlay();
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

// ---- Skill / combat viewer (stage-ordered full-cast timeline) ---------------

function isCombatMode() {
  const r = document.querySelector('input[name="mode"]:checked');
  return !!r && r.value === "combat";
}

async function loadSpellList() {
  try {
    spellCatalog = await client.callTool("spell.list", {});
  } catch (e) {
    console.warn("spell.list failed:", e.message);
    spellCatalog = null;
  }
}

function combatList() {
  if (!spellCatalog) return [];
  const src = $("combatSource").value;
  if (src === "monster") return spellCatalog.monster_attacks || [];
  if (src === "ability") return spellCatalog.abilities || [];
  if (src === "trap") return spellCatalog.traps || [];
  return spellCatalog.spells || [];
}

// 显示名：优先中文名 name_zh，仅在中文名与英文名不同（即确有翻译）时附英文名；
// 怪物远程攻击通常无翻译，回落到英文名。description 用作悬浮提示。
function combatLabel(e) {
  if (!e) return "";
  const zh = e.name_zh && e.name_zh !== e.name ? e.name_zh : "";
  const base = zh ? `${zh} (${e.name})` : e.name;
  return e.variant ? `${base} · ${e.variant}` : base;
}

// 在信息区展示原英文名与合成中文描述（用 textContent 防注入）。
function renderCombatInfo() {
  const el = $("combatInfo");
  if (!combatEntry) { el.hidden = true; el.textContent = ""; return; }
  el.hidden = false;
  el.textContent = "";
  const addRow = (k, v) => {
    if (!v) return;
    const row = document.createElement("div");
    row.className = "ci-row";
    const kk = document.createElement("span");
    kk.className = "ci-key"; kk.textContent = k + "：";
    const vv = document.createElement("span");
    vv.className = "ci-val"; vv.textContent = v;
    row.appendChild(kk); row.appendChild(vv); el.appendChild(row);
  };
  addRow("英文名", combatEntry.name);
  if (combatEntry.name_zh && combatEntry.name_zh !== combatEntry.name)
    addRow("中文名", combatEntry.name_zh);
  if (combatEntry.level)
    addRow("法术等级", combatEntry.level);
  if (combatEntry.variant) addRow("攻击形态", combatEntry.variant);
  if (combatEntry.condition) addRow("触发条件", combatEntry.condition);
  if (combatEntry.trigger) addRow("触发条件", combatEntry.trigger);
  if (combatEntry.source_ref) addRow("源码位置", combatEntry.source_ref);
  if (Array.isArray(combatEntry.objects) && combatEntry.objects.length)
    addRow("宿主对象", combatEntry.objects.join(" / "));
  if (Array.isArray(combatEntry.mana_by_level) && combatEntry.mana_by_level.length) {
    const txt = combatEntry.mana_by_level
      .map((o) => `${o.level}级=${o.mana}`)
      .join("，");
    addRow("各级法力", txt);
  }
  // 各级视觉参数（持续时间 / 范围 / 弹射物数量 / 弹速 / 法术特有字段）
  if (Array.isArray(combatEntry.visual_by_level) && combatEntry.visual_by_level.length) {
    const wrap = document.createElement("div");
    wrap.className = "ci-visual";
    const title = document.createElement("div");
    title.className = "ci-key"; title.textContent = "各级视觉参数：";
    wrap.appendChild(title);

    const keys = [];
    let note = "";
    for (const o of combatEntry.visual_by_level) {
      for (const k of Object.keys(o.params || {})) {
        if (k === "_note") { note = o.params[k]; continue; }
        if (!keys.includes(k)) keys.push(k);
      }
    }
    const table = document.createElement("table");
    table.className = "ci-vtable";
    const thead = document.createElement("tr");
    const th0 = document.createElement("th"); th0.textContent = "级别"; thead.appendChild(th0);
    for (const k of keys) {
      const th = document.createElement("th"); th.textContent = k; thead.appendChild(th);
    }
    table.appendChild(thead);
    for (const o of combatEntry.visual_by_level) {
      const tr = document.createElement("tr");
      const td0 = document.createElement("td"); td0.textContent = o.level; tr.appendChild(td0);
      for (const k of keys) {
        const td = document.createElement("td");
        td.textContent = (o.params && o.params[k] != null) ? o.params[k] : "—";
        tr.appendChild(td);
      }
      table.appendChild(tr);
    }
    wrap.appendChild(table);
    if (note) {
      const n = document.createElement("div");
      n.className = "ci-note"; n.textContent = "注：" + note;
      wrap.appendChild(n);
    }
    el.appendChild(wrap);
  } else {
    addRow("各级视觉参数", "（无级别视觉缩放：单级/实用法术）");
  }
  if (combatEntry.description) {
    const d = document.createElement("div");
    d.className = "ci-desc";
    const kk = document.createElement("span");
    kk.className = "ci-key"; kk.textContent = "描述：";
    const vv = document.createElement("span");
    vv.className = "ci-val"; vv.textContent = combatEntry.description;
    d.appendChild(kk); d.appendChild(vv); el.appendChild(d);
  }
}

function populateCombatSpells() {
  const sel = $("combatSpell");
  sel.innerHTML = "";
  for (const e of combatList()) {
    const o = document.createElement("option");
    o.value = e.id;
    o.textContent = combatLabel(e) + (e.monster_ai && !e.variant ? `  (${e.monster_ai})` : "");
    o.title = e.description || e.name;
    sel.appendChild(o);
  }
}

function populateCombatClass() {
  const sel = $("combatClass");
  sel.innerHTML = "";
  for (const cp of (charIndex ? [...charIndex.keys()].sort() : [])) {
    const o = document.createElement("option");
    o.value = cp;
    o.textContent = cp;
    sel.appendChild(o);
  }
}

function populateCombatArchive() {
  const sel = $("combatArchive");
  sel.innerHTML = "";
  for (const a of allArchives) {
    const o = document.createElement("option");
    o.value = a;
    o.textContent = basename(a);
    o.title = a;
    sel.appendChild(o);
  }
}

function fillCombatMonsters() {
  const sel = $("combatMonster");
  sel.innerHTML = "";
  for (const c of (monsterMap ? [...monsterMap.keys()].sort() : [])) {
    const o = document.createElement("option");
    o.value = c;
    o.textContent = c;
    sel.appendChild(o);
  }
  fillCombatMonsterAnims();
}

function fillCombatMonsterAnims() {
  const sel = $("combatMonsterAnim");
  sel.innerHTML = "";
  const list = (monsterMap && monsterMap.get($("combatMonster").value)) || [];
  for (const s of list) {
    const o = document.createElement("option");
    o.value = s.name;
    o.textContent = `${s.anim || s.type} (${s.frames_per_dir || "?"} 帧/方向)`;
    o.title = s.name;
    sel.appendChild(o);
  }
}

function combatArchiveFor() {
  return $("combatArchive").value || allArchives[0] || "";
}

// The backend only reports `directions` for monster sprites; player CL2 are
// fixed 8-direction and missile graphics are single-direction.
function combatDirsOf(name, res) {
  const d = (res && res.directions > 0) ? res.directions : 0;
  if (d) return d;
  const first = String(name).split(/[\\/]/)[0].toLowerCase();
  return first === "plrgfx" ? 8 : 1;
}

async function decodeCombatFrames(name, width, trn) {
  const key = name + "|" + (width === "auto" ? "auto" : width) + "|" + (trn || "");
  if (combatCache.has(key)) return combatCache.get(key);
  const args = buildDecodeArgs(
    { name, archive_path: combatArchiveFor() }, name, { width, trn });
  const res = await client.callTool("sprite.decode", args);
  const fs = makeFrameSet((res && res.frames) || [], combatDirsOf(name, res));
  combatCache.set(key, fs);
  return fs;
}

// Not every class has the same armour/weapon combos (e.g. sorcerer/barbarian
// have no light-armour + no-weapon "ln"). Pick a combo that exists for BOTH the
// stand and the casting action so the player keeps the same look across stages.
function combatCombo(e, action) {
  const st = e.combos.get("st") || new Set();
  const ca = e.combos.get(action) || new Set();
  const common = [...st].filter((c) => ca.has(c)).sort();
  if (common.length) return common[0];
  return [...st].sort()[0] || "ln";
}

function plrgfxPath(cp, e, combo, action) {
  const prefix = e.classChar + combo;
  return `plrgfx\\${cp}\\${prefix}\\${prefix}${action}.cl2`;
}

// A missile sprite's numbered .cl2 files are its DIRECTIONS (DevilutionX
// `animFAmt` / `spritesForDirection(Direction16)`), not consecutive animation
// frames. Keep each file as one direction group so playback stays in one dir.
async function decodeLayerFrames(layer) {
  const key = "layer|" + (layer.files || []).join("|") + "|" + layer.width;
  if (combatCache.has(key)) return combatCache.get(key);
  const groups = [];
  for (const f of layer.files || []) {
    const one = await decodeCombatFrames(f, layer.width);
    groups.push(one.groups[0] || []);
  }
  const fs = { groups: groups.length ? groups : [[]],
               directions: groups.length || 1 };
  combatCache.set(key, fs);
  return fs;
}

function combatDebuffTrn() {
  const st = ((combatEntry && combatEntry.stages) || []).find((s) => s.id === "debuff");
  if (!st) return null;
  for (const l of st.layers || [])
    if (l.kind === "trn" && l.trn && l.trn.name) return l.trn.name;
  return null;
}

async function buildCombatScene() {
  if (!isCombatMode() || !combatEntry) return;
  setMeta("loading combat scene…");
  stopPlay();
  try {
    const cp = $("combatClass").value;
    const ce = charIndex && charIndex.get(cp);
    combatPlayerStandName = "";
    if (ce) {
      const action = combatEntry.cast_action || "qm";
      const combo = combatCombo(ce, action);
      combatPlayerStandName = plrgfxPath(cp, ce, combo, "st");
      combatActors.playerStand =
        await decodeCombatFrames(combatPlayerStandName, "auto");
      try {
        combatActors.playerCast =
          await decodeCombatFrames(plrgfxPath(cp, ce, combo, action), "auto");
      } catch (e) {
        combatActors.playerCast = null;   // fall back to standing if unavailable
      }
      if (frameSetEmpty(combatActors.playerCast))
        combatActors.playerCast = combatActors.playerStand;
    }

    const mName = $("combatMonsterAnim").value;
    const trn = combatDebuffTrn();
    combatActors.monsterBase = mName
      ? await decodeCombatFrames(mName, "auto") : makeFrameSet([], 1);

    // The caster is whoever casts (player for spells, monster for ranged
    // attacks); the target is the other actor. Debuff TRN tints the target.
    const targetIsMonster = combatCasterAnchor() === "player";
    const targetName = targetIsMonster ? mName : combatPlayerStandName;
    const tinted = (targetName && trn)
      ? await decodeCombatFrames(targetName, "auto", trn) : null;
    combatActors.playerTrn = (!targetIsMonster && tinted) ? tinted : makeFrameSet([], 1);
    combatActors.monsterTrn = (targetIsMonster && tinted) ? tinted : makeFrameSet([], 1);

    combatStageFrames = [];
    for (const st of combatEntry.stages || []) {
      const layers = [];
      for (const l of st.layers || []) {
        if (l.kind !== "sprite") continue;
        layers.push({ frameSet: await decodeLayerFrames(l), anchor: l.anchor, role: l.role });
      }
      combatStageFrames.push(layers);
    }
    buildCombatTimeline();
    hideError();
    syncCombatDirControl();
    renderCombatStage(0, 0);
  } catch (e) {
    showError("战斗场景加载失败：" + (e.message || e));
  }
}

function stageFrameCount(si) {
  const st = ((combatEntry && combatEntry.stages) || [])[si];
  if (!st) return 1;
  if (st.id === "cast")
    return frameSetBudget(frameSetEmpty(combatActors.playerCast)
                          ? combatActors.playerStand : combatActors.playerCast);
  let n = 1;
  for (const l of combatStageFrames[si] || [])
    n = Math.max(n, frameSetBudget(l.frameSet));
  return Math.max(1, n);
}

function buildCombatTimeline() {
  combatTimeline = [];
  ((combatEntry && combatEntry.stages) || []).forEach((st, si) => {
    const n = stageFrameCount(si);
    for (let f = 0; f < n; f++) combatTimeline.push({ stageIdx: si, frameIdx: f });
  });
  combatPlayIdx = 0;
}

const COMBAT_W = 480, COMBAT_H = 320;
// Screen-space direction vectors (y grows downward), 8-way like Diablo sprites.
const COMBAT_DIR_VEC = [
  [0, 1], [-1, 1], [-1, 0], [-1, -1],
  [0, -1], [1, -1], [1, 0], [1, 1],
];

// spell.list marks the caster explicitly (cast_action.anchor): player for
// spells, monster for ranged attacks. The other actor is the target.
function combatCasterAnchor() {
  const cast = ((combatEntry && combatEntry.stages) || []).find((s) => s.id === "cast");
  const cl = cast && (cast.layers || []).find((l) => l.kind === "cast_action");
  return (cl && cl.anchor) || "player";
}

function syncCombatDirControl() {
  combatDir = Math.max(0, Math.min(7, combatDir | 0));
  $("dir").min = "0";
  $("dir").max = "7";
  $("dir").value = String(combatDir);
  $("dirNo").textContent = String(combatDir);
  // The per-sprite "方向数" selector has no meaning for a two-actor scene.
  $("directions").disabled = true;
}

// The caster is the fixed centre of the scene. The target is placed along the
// selected direction and turned to face back at the caster; the caster faces
// the target. Both positions AND both facing directions are recomputed here.
function combatLayout() {
  const d = ((combatDir % 8) + 8) % 8;
  const [vx, vy] = COMBAT_DIR_VEC[d];
  const m = Math.hypot(vx, vy) || 1;
  const cx = COMBAT_W / 2, cy = COMBAT_H / 2;
  return {
    caster: { x: cx, y: cy },
    target: { x: cx + (vx / m) * 134, y: cy + (vy / m) * 78 },
    casterDir: d,                 // caster faces the target
    targetDir: (d + 4) % 8,       // target faces the caster
  };
}

function pushCombatActor(actors, fs, center, sceneDir, fi) {
  const f = frameSetAt(fs, fi, sceneDir);
  if (!f) return;
  actors.push({
    rgba: f.rgba_base64, w: f.width, h: f.height,
    x: Math.round(center.x - f.width / 2),
    y: Math.round(center.y - f.height / 2),
    z: 1 + center.y / 1000,       // lower on screen = drawn in front
  });
}

function renderCombatStage(si, fi) {
  if (!combatEntry) return;
  const stages = combatEntry.stages || [];
  if (!stages.length) return;
  si = Math.max(0, Math.min(si, stages.length - 1));
  const st = stages[si];
  combatStage = si;
  combatFrame = fi;
  const L = combatLayout();
  const casterAnchor = combatCasterAnchor();
  const targetAnchor = casterAnchor === "player" ? "monster" : "player";
  const debuffIdx = stages.findIndex((s) => s.id === "debuff");
  const tinted = debuffIdx >= 0 && si >= debuffIdx;
  const actors = [];

  const actorFs = (anchor, onCastStage) => {
    if (anchor === "player") {
      if (tinted && !frameSetEmpty(combatActors.playerTrn)) return combatActors.playerTrn;
      return onCastStage ? combatActors.playerCast : combatActors.playerStand;
    }
    if (tinted && !frameSetEmpty(combatActors.monsterTrn)) return combatActors.monsterTrn;
    return combatActors.monsterBase;
  };
  pushCombatActor(actors, actorFs(casterAnchor, st.id === "cast"),
                  L.caster, L.casterDir, fi);
  pushCombatActor(actors, actorFs(targetAnchor, false),
                  L.target, L.targetDir, fi);

  // Effect layers: projectiles travel caster -> target, impacts/debuffs land on
  // the target. Every layer plays inside the current direction's frame group.
  const total = Math.max(1, stageFrameCount(si));
  for (const l of combatStageFrames[si] || []) {
    const f = frameSetAt(l.frameSet, fi, combatDir);
    if (!f) continue;
    let cx, cy;
    if (l.role === "projectile") {
      const t = total <= 1 ? 0.5 : (fi % total) / (total - 1);
      cx = L.caster.x + (L.target.x - L.caster.x) * t;
      cy = L.caster.y + (L.target.y - L.caster.y) * t;
    } else if (l.role === "caster") {
      cx = L.caster.x; cy = L.caster.y;
    } else if (l.role === "impact" || l.role === "debuff") {
      cx = L.target.x; cy = L.target.y;
    } else {
      const c = (l.anchor === casterAnchor) ? L.caster : L.target;
      cx = c.x; cy = c.y;
    }
    actors.push({ rgba: f.rgba_base64, w: f.width, h: f.height,
                  x: Math.round(cx - f.width / 2),
                  y: Math.round(cy - f.height / 2),
                  z: 2 + cy / 1000 });
  }

  drawScene($("canvas"), COMBAT_W, COMBAT_H, actors, zoom);
  $("dirNo").textContent = String(combatDir);
  $("frameNo").textContent = `dir ${combatDir} · ${st.label} · 帧 ${fi} / ${total - 1}`;
  $("stageNo").textContent = `${si + 1}/${stages.length} ${st.label}`;
  $("frame").max = String(total - 1);
  $("frame").value = String(Math.min(fi, total - 1));
  $("currentName").textContent = `${combatLabel(combatEntry)} — ${st.label}`;
  renderCombatInfo();
  setMeta(`技能/战斗查看器  ${combatLabel(combatEntry)}  ·  ${st.label}  ·  ${st.layers.length} 层  ·  方向 ${combatDir}`);
}

function startCombatPlay() {
  if (!combatEntry) return;
  if ($("combatLoop").checked && !combatTimeline.length) return;
  isPlaying = true;
  const fps = Math.min(60, Math.max(1, Number($("fps").value) || 12));
  playTimer = setInterval(() => {
    if ($("combatLoop").checked) {
      combatPlayIdx = (combatPlayIdx + 1) % combatTimeline.length;
      const step = combatTimeline[combatPlayIdx];
      renderCombatStage(step.stageIdx, step.frameIdx);
    } else {
      const n = Math.max(1, stageFrameCount(combatStage));
      renderCombatStage(combatStage, (combatFrame + 1) % n);
    }
  }, 1000 / fps);
  $("playBtn").textContent = "⏸ 暂停";
}

function selectCombatEntry() {
  const list = combatList();
  combatEntry = list.find((e) => e.id === $("combatSpell").value) || list[0] || null;
  combatStage = 0;
  combatFrame = 0;
  combatPlayIdx = 0;
}

async function showCombatView() {
  stopPlay();
  syncCombatDirControl();
  if (!spellCatalog) await loadSpellList();
  if (!spellCatalog) {
    showError("spell.list 不可用（后端未注册或 DevilutionX 数据表缺失）。");
    return;
  }
  if (!monsterMap) await populateMonsters();
  populateCombatClass();
  populateCombatArchive();
  populateCombatSpells();
  fillCombatMonsters();
  selectCombatEntry();
  await buildCombatScene();
}

function stepCombatStage(delta) {
  if (!combatEntry) return;
  stopPlay();
  const n = combatEntry.stages.length;
  renderCombatStage((combatStage + delta + n) % n, 0);
}

// ---- Scene viewer (5th browse mode) -----------------------------------------
//
// The backend scene.build returns two pixel-aligned PNGs (a bounded preview
// and a transparent SOL overlay) as base64. We decode each into a bitmap and
// drawImage() it onto one of two stacked <canvas> layers, then apply ONE
// transform to the shared container, so zoom/pan never desync them. There is no
// <img>/src involved — the bitmap is drawn, never referenced by a URL.

const sceneState = {
  zoom: 2,
  panX: 0,
  panY: 0,
  w: 0,             // preview natural width
  h: 0,             // preview natural height
  loaded: false,
  loading: false,
  dragging: false,
  dragX: 0,
  dragY: 0,
};
let sceneCatalog = [];

// Scene PNGs are drawn onto <canvas> layers, exactly like viewer.js blits
// decoded sprite frames. We decode the base64 PNG bytes into a bitmap and
// drawImage() it — there is NO <img> and NO `src` to go missing. This also
// avoids ever building a giant `data:` URL.
function sceneBase64ToBytes(b64) {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

async function sceneDecodeBitmap(b64) {
  const blob = new Blob([sceneBase64ToBytes(b64)], { type: "image/png" });
  if (typeof createImageBitmap === "function") {
    return { bitmap: await createImageBitmap(blob), revoke: null, close: true };
  }
  // Fallback: an off-DOM Image with a REAL object-URL src (never appended to the
  // document, so no src-less <img> is ever shown).
  const url = URL.createObjectURL(blob);
  try {
    const img = await new Promise((resolve, reject) => {
      const el = new Image();
      el.onload = () => resolve(el);
      el.onerror = () => reject(new Error("场景 PNG 解码失败"));
      el.src = url;
    });
    return { bitmap: img, revoke: url, close: false };
  } catch (e) {
    URL.revokeObjectURL(url);
    throw e;
  }
}

// Decode a base64 PNG and blit it onto `canvas` at native pixel size. Returns
// the bitmap's natural dimensions (used for zoom/pan). An empty value clears
// the canvas instead of leaving stale pixels behind.
async function sceneDrawPng(canvas, b64) {
  if (!b64) {
    const ctx = canvas.getContext("2d");
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    canvas.width = 0;
    canvas.height = 0;
    return { w: 0, h: 0 };
  }
  const { bitmap, revoke, close } = await sceneDecodeBitmap(b64);
  try {
    const w = bitmap.width || bitmap.naturalWidth;
    const h = bitmap.height || bitmap.naturalHeight;
    canvas.width = w;
    canvas.height = h;
    const ctx = canvas.getContext("2d");
    ctx.imageSmoothingEnabled = false;
    ctx.clearRect(0, 0, w, h);
    ctx.drawImage(bitmap, 0, 0);
    return { w, h };
  } finally {
    if (close && typeof bitmap.close === "function") bitmap.close();
    if (revoke) URL.revokeObjectURL(revoke);
  }
}

function applySceneTransform() {
  if (!sceneState.loaded) return;
  const z = sceneState.zoom;
  const layers = $("sceneLayers");
  layers.style.width = `${sceneState.w * z}px`;
  layers.style.height = `${sceneState.h * z}px`;
  layers.style.left = `${sceneState.panX}px`;
  layers.style.top = `${sceneState.panY}px`;
  // Both images share the layer box; only the container is scaled/positioned.
  for (const id of ["sceneBase", "sceneOverlay"]) {
    const el = $(id);
    el.style.width = `${sceneState.w * z}px`;
    el.style.height = `${sceneState.h * z}px`;
  }
  $("sceneZoomNo").textContent = `${z}×`;
}

function sceneFit() {
  const vp = $("sceneViewport");
  if (!sceneState.loaded) return;
  // Fit the whole preview into the viewport at zoom 1, centred.
  const z = 1;
  sceneState.zoom = z;
  $("sceneZoom").value = String(z);
  sceneState.panX = Math.max(0, (vp.clientWidth - sceneState.w * z) / 2);
  sceneState.panY = Math.max(0, (vp.clientHeight - sceneState.h * z) / 2);
  applySceneTransform();
}

async function loadSceneCatalog() {
  try {
    const res = await client.callTool("scene.list", {});
    sceneCatalog = (res && res.scenes) || [];
    const sel = $("sceneSel");
    sel.innerHTML = "";
    for (const s of sceneCatalog) {
      const o = document.createElement("option");
      o.value = s.id;
      o.textContent = `${s.title}  (${s.grid_width}×${s.grid_height})`;
      sel.appendChild(o);
    }
    if (sceneCatalog.length) sel.value = "tristram";
  } catch (e) {
    showError("scene.list 失败: " + (e.message || e));
  }
}

async function loadScene() {
  if (sceneState.loading) return;
  const id = $("sceneSel").value || "tristram";
  hideError();
  setMeta(`加载场景 ${id} …`);
  $("sceneLoadBtn").disabled = true;
  sceneState.loading = true;
  try {
    const args = { id };
    // Prefer an explicit path; otherwise the catalog's primary archive. The
    // full archive stack is always passed so town/L1 assets resolve even when
    // split across MPQs.
    const override = ($("sceneArchive").value || "").trim();
    const primary = override || ((allArchives && allArchives.length) ? allArchives[0] : null);
    if (primary) args.archive = primary;
    if (allArchives && allArchives.length) args.archives = allArchives.slice();
    if (!args.archive) {
      showError("未连接或未发现 MPQ 档案：请在上方填入 MPQ 路径");
      return;
    }
    args.preview_max_width = 1000;
    args.lighting = !!$("sceneLightToggle").checked;
    const res = await client.callTool("scene.build", args);
    if (!res || !res.image_base64) throw new Error("scene.build 未返回图像");
    // Draw the returned PNG bitmaps onto the two <canvas> layers. No <img>/src is
    // involved: the decoded bitmap is blitted directly, so the canvas always
    // shows the actual scene once the tool returns.
    const dim = await sceneDrawPng($("sceneBase"), res.image_base64);
    await sceneDrawPng($("sceneOverlay"), res.overlay_base64 || "");
    $("sceneOverlay").hidden = !res.overlay_base64 || !$("sceneOverlayToggle").checked;
    // Night-palette scenes (e.g. Tristram) render very dark and look blank;
    // the brighten toggle defaults to ON, so apply its state on every load.
    $("sceneLayers").classList.toggle("bright", $("sceneBrightToggle").checked);
    sceneState.w = dim.w || res.preview_width || 0;
    sceneState.h = dim.h || res.preview_height || 0;
    sceneState.loaded = true;
    sceneFit();
    setMeta(`${res.title}  原始 ${res.width}×${res.height}  预览 ${res.preview_width}×${res.preview_height}  ` +
            `网格 ${res.grid_width}×${res.grid_height}  缩放 1/${res.scale}  ` +
            `透明像素 ${res.transparent_pixel_count}  Solid ${res.solid_count}  ` +
            `光照 ${res.lighting ? "开" : "关"}`);
  } catch (e) {
    showError("scene.build 失败: " + (e.message || e));
    setMeta("场景加载失败", true);
  } finally {
    sceneState.loading = false;
    $("sceneLoadBtn").disabled = false;
  }
}

async function showSceneView() {
  // Auto-load the default scene on first entry so the canvas is never an empty
  // element with no content (the src-less <img> problem). Catalog + archives are
  // both required; the WS connect handler below triggers this too.
  if (!sceneCatalog.length) await loadSceneCatalog();
  if (sceneState.loaded) { sceneFit(); return; }
  if (allArchives && allArchives.length && !sceneState.loading) {
    await loadScene();
  }
}

function wireSceneControls() {
  $("sceneLoadBtn").addEventListener("click", () => loadScene());
  $("sceneSel").addEventListener("change", () => { /* explicit load button */ });
  $("sceneZoom").addEventListener("input", (e) => {
    sceneState.zoom = Math.max(1, Math.min(8, parseInt(e.target.value, 10) || 1));
    applySceneTransform();
  });
  $("sceneOverlayToggle").addEventListener("change", (e) => {
    $("sceneOverlay").hidden = !e.target.checked;
  });
  $("sceneBrightToggle").addEventListener("change", (e) => {
    $("sceneLayers").classList.toggle("bright", e.target.checked);
  });
  $("sceneLightToggle").addEventListener("change", () => {
    // Rebuild the scene so dLight shading is applied/removed server-side.
    if (sceneState.loaded) loadScene();
  });
  $("sceneFitBtn").addEventListener("click", () => sceneFit());

  const vp = $("sceneViewport");
  vp.addEventListener("wheel", (e) => {
    if (!isSceneMode()) return;
    e.preventDefault();
    const step = e.deltaY < 0 ? 1 : -1;
    sceneState.zoom = Math.max(1, Math.min(8, sceneState.zoom + step));
    $("sceneZoom").value = String(sceneState.zoom);
    applySceneTransform();
  }, { passive: false });
  vp.addEventListener("mousedown", (e) => {
    if (!isSceneMode()) return;
    sceneState.dragging = true;
    sceneState.dragX = e.clientX - sceneState.panX;
    sceneState.dragY = e.clientY - sceneState.panY;
    vp.classList.add("dragging");
  });
  window.addEventListener("mousemove", (e) => {
    if (!sceneState.dragging) return;
    sceneState.panX = e.clientX - sceneState.dragX;
    sceneState.panY = e.clientY - sceneState.dragY;
    applySceneTransform();
  });
  window.addEventListener("mouseup", () => {
    sceneState.dragging = false;
    vp.classList.remove("dragging");
  });
}

// ---- GUI layout viewer (6th browse mode) -----------------------------------
//
// Backend ui.build returns a 640x480 PNG of REAL MPQ art (background / panel /
// orbs / buttons) plus an aligned element-box overlay PNG, as base64. We reuse
// sceneDrawPng() to blit both onto stacked canvases. A THIRD canvas holds the
// text layer: because the game's bitmap font only has basic-ASCII glyphs in the
// MPQ, the backend does NOT burn text into the PNG. Instead we draw each Text /
// Button element's label with an approximate system font using the structured
// text/font_size/color/align/flags fields from ui.build.elements. This layer is
// toggleable so the pure "real art" composite can be inspected on its own.

const GUI_INFO_PROMPT = "<em>从上方「元素」下拉框选择元素查看详情…</em>";
const guiState = {
  catalog: [],        // ui.list screens
  elements: [],       // ui.build elements for the current screen
  selectedIndex: -1,  // dropdown-selected element (re-highlighted on zoom)
  screenId: "",       // screen whose elements are currently shown
  zoom: 2,
  loaded: false,
  loading: false,
};

// font_size (DevilutionX GameFontTables) -> approximate CSS px. Game fonts are
// ~22/24/30/42/46 tall; a slightly smaller CSS size keeps lines inside rects.
function guiFontSize(fs) {
  const map = { 12: 11, 22: 15, 24: 18, 30: 23, 42: 32, 46: 36 };
  return map[fs] || (fs ? Math.round(fs * 0.75) : 12);
}

// color = fonts\\<name>.trn basename -> representative hex. Approximation only.
function guiColor(name) {
  const map = {
    goldui: "#d0a830", grayui: "#c8c8c8", golduis: "#8a7430", grayuis: "#888888",
    yellow: "#ffd24a", white: "#f0f0f0", whitegold: "#ffe9b0",
    red: "#e04848", blue: "#7090e8", orange: "#f09040", black: "#181818",
    buttonface: "#b0a890", buttonpushed: "#706858",
    gamedialogwhite: "#f0f0f0", gamedialogyellow: "#ffd24a", gamedialogred: "#e04848",
  };
  return map[name] || "#e6e6ee";
}

// Replace {PLACEHOLDER} tokens with example values so a screen reads like a
// real game frame. These are static examples, NOT live runtime data.
function guiExampleText(text) {
  const ex = {
    "{PLAYER_NAME}": "Aldric", "{CLASS}": "Warrior", "{VERSION}": "Diablo v1.09",
    "{HP}": "182", "{HP_MAX}": "182", "{MANA}": "77", "{MANA_MAX}": "77",
    "{VAL}": "48", "{GOLD}": "5210", "{SPELL}": "Firebolt",
    "{TITLE}": "Warning", "{BODY}": "Your inventory is full.",
    "{QUEST_TITLE}": "The Butcher", "{DESCRIPTION}": "Option description",
    "{CREDITS_LINES}": "Blizzard Entertainment\nDesign & Programming",
  };
  let out = String(text == null ? "" : text);
  for (const [k, v] of Object.entries(ex)) out = out.split(k).join(v);
  return out;
}

// Draw the text layer onto a native 640x480 canvas. Elements are drawn in z
// order; only Text and Button (and any element carrying text) are rendered.
function drawGuiText(canvas, elements) {
  const W = 640, H = 480;
  canvas.width = W;
  canvas.height = H;
  const ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, W, H);
  if (!$("guiTextToggle").checked) return;   // toggle: pure art + boxes
  const list = (elements || []).slice().sort((a, b) => (a.z || 0) - (b.z || 0));
  for (const el of list) {
    if (!el.text) continue;
    const r = el.rect || [0, 0, 0, 0];
    const [x, y, w, h] = r;
    if (w <= 0 || h <= 0) continue;
    const fs = guiFontSize(el.font_size);
    ctx.font = `600 ${fs}px ui-monospace, Menlo, Consolas, monospace`;
    ctx.fillStyle = guiColor(el.color);
    ctx.textBaseline = "middle";
    // alignment from flags / align field
    const center = (el.flags && el.flags.align_center) || el.align === "center";
    const right = (el.flags && el.flags.align_right) || el.align === "right";
    ctx.textAlign = right ? "right" : center ? "center" : "left";
    const tx = right ? x + w - 3 : center ? x + w / 2 : x + 3;
    const ty = (el.flags && el.flags.vertical_center) ? y + h / 2 : y + Math.min(h / 2, fs);
    const lines = guiExampleText(el.text).split("\n");
    const lh = Math.max(fs + 2, Math.round(fs * 1.15));
    const startY = ty - ((lines.length - 1) * lh) / 2;
    for (let i = 0; i < lines.length; i++) {
      // Clip text to the element's rect so long menu lines never overflow it.
      ctx.save();
      ctx.beginPath();
      ctx.rect(Math.max(0, x), Math.max(0, y), Math.min(W - x, w), Math.min(H - y, h));
      ctx.clip();
      ctx.fillText(lines[i], tx, startY + i * lh);
      ctx.restore();
    }
  }
}

// Apply the shared CSS zoom to all three canvases + the layers container.
function applyGuiTransform() {
  const z = guiState.zoom;
  const w = 640 * z, h = 480 * z;
  $("guiLayers").style.width = `${w}px`;
  $("guiLayers").style.height = `${h}px`;
  for (const id of ["guiBase", "guiText", "guiOverlay"]) {
    const el = $(id);
    el.style.width = `${w}px`;
    el.style.height = `${h}px`;
  }
}

async function loadGuiCatalog() {
  try {
    const res = await client.callTool("ui.list", {});
    guiState.catalog = (res && res.screens) || [];
    const sel = $("guiSel");
    sel.innerHTML = "";
    for (const s of guiState.catalog) {
      const o = document.createElement("option");
      o.value = s.id;
      o.textContent = `${s.title}  (${s.element_count} 元素)`;
      sel.appendChild(o);
    }
    if (guiState.catalog.length) sel.value = "mainmenu";
  } catch (e) {
    showError("ui.list 失败: " + (e.message || e));
  }
}

async function loadGuiScreen() {
  const id = $("guiSel").value || "mainmenu";
  // Preserve the selected element only when reloading the SAME screen (number
  // tags toggle / manual reload); a genuine screen switch starts unselected.
  const preserveIndex = (id === guiState.screenId) ? guiState.selectedIndex : -1;
  if (guiState.loading) {
    // A newer request arrives while a build is in flight: don't run a second
    // concurrent build. `superseded()` in `finally` detects the selection
    // change and re-issues the load for the current screen.
    return;
  }
  hideError();
  setMeta(`加载界面 ${id} …`);
  $("guiLoadBtn").disabled = true;
  guiState.loading = true;
  const superseded = () => ($("guiSel").value || "mainmenu") !== id;
  try {
    const args = { id, overlay: true, label_numbers: !!$("guiNumberToggle").checked,
                   out: "out/ui" };
    // Same archive-stack logic as the scene viewer: explicit override or the
    // catalog's primary archive, plus the full stack so cross-archive art
    // (DIABDAT .pcx/.cel + devilutionx .pal/.clx) all resolves.
    const override = ($("guiArchive").value || "").trim();
    const primary = override || ((allArchives && allArchives.length) ? allArchives[0] : null);
    if (primary) args.archive = primary;
    if (allArchives && allArchives.length) args.archives = allArchives.slice();
    if (!args.archive) {
      clearGuiElementState();
      showError("未连接或未发现 MPQ 档案：请在上方填入 MPQ 路径");
      setMeta("界面加载失败", true);
      return;
    }
    const res = await client.callTool("ui.build", args);
    if (!res || !res.image_base64) throw new Error("ui.build 未返回图像");
    if (superseded()) return; // newer screen requested; re-issued in `finally`
    await sceneDrawPng($("guiBase"), res.image_base64);
    await sceneDrawPng($("guiOverlay"), res.overlay_base64 || "");
    if (superseded()) return;
    // Only accept an array; keep entries as-is so dropdown numbering still
    // matches the backend overlay number tags (malformed entries are handled
    // defensively when rendered).
    guiState.elements = Array.isArray(res.elements) ? res.elements : [];
    populateGuiElementSelect(preserveIndex);
    drawGuiText($("guiText"), guiState.elements);
    $("guiOverlay").hidden = !res.overlay_base64 || !$("guiOverlayToggle").checked;
    guiState.screenId = id;
    guiState.loaded = true;
    applyGuiTransform();
    const pals = (res.palettes || []).map((p) => p.name.split(/[\\/]/).pop()).join(", ") || "embedded";
    setMeta(`${res.title}  640×480  元素 ${res.element_count}  框 ${res.overlay_box_count}  ` +
            `美术解码 ok=${res.art_ok_count} 缺=${res.art_missing_count} 错=${res.art_error_count}  ` +
            `调色板 [${pals}]  透明像素 ${res.transparent_pixel_count}`);
  } catch (e) {
    clearGuiElementState();
    showError("ui.build 失败: " + (e.message || e));
    setMeta("界面加载失败", true);
  } finally {
    guiState.loading = false;
    $("guiLoadBtn").disabled = false;
    // If the screen selection moved on during the in-flight build, build the
    // current screen now instead of leaving the UI on a stale screen.
    if (superseded()) loadGuiScreen();
  }
}

async function showGuiView() {
  if (!guiState.catalog.length) await loadGuiCatalog();
  if (guiState.loaded) { applyGuiTransform(); return; }
  if (allArchives && allArchives.length && !guiState.loading) {
    await loadGuiScreen();
  }
}

// Reset the element dropdown / detail pane / highlight box to the empty state.
// Called on init and on any failed or aborted build, so stale per-screen data
// from a previous screen never lingers when a load does not succeed.
function clearGuiElementState() {
  guiState.elements = [];
  guiState.selectedIndex = -1;
  guiState.screenId = "";
  guiState.loaded = false;
  populateGuiElementSelect();
}

// Populate the element dropdown from the current screen's ui.build elements.
// ALL elements are listed (including text-only ones with no backend art), so
// users can inspect any element without having to hover its (possibly tiny or
// overlapping) rectangle on the canvas. `preserveIndex` restores a selection
// when reloading the SAME screen (number-tag toggle / manual reload).
function populateGuiElementSelect(preserveIndex = -1) {
  const sel = $("guiElementSel");
  sel.innerHTML = "";
  const ph = document.createElement("option");
  ph.value = "";
  ph.textContent = "— 选择元素 —";
  sel.appendChild(ph);
  // Keep entries as-is (no filtering) so the 1..N numbering still matches the
  // backend overlay number tags; just render defensively if a field is missing.
  guiState.elements.forEach((el, i) => {
    const o = document.createElement("option");
    o.value = String(i);
    const label = (el && (el.label || el.id)) || `元素 ${i + 1}`;
    const type = (el && el.type) || "?";
    o.textContent = `${i + 1}. ${label}  [${type}]`;
    sel.appendChild(o);
  });
  sel.value = "";
  guiState.selectedIndex = -1;
  showGuiElementDetail(null);
  if (Number.isInteger(preserveIndex) && preserveIndex >= 0 &&
      preserveIndex < guiState.elements.length && guiState.elements[preserveIndex]) {
    sel.value = String(preserveIndex);
    guiState.selectedIndex = preserveIndex;
    showGuiElementDetail(guiState.elements[preserveIndex]);
  }
}

// Show details + highlight for the dropdown-selected element (or clear them).
function selectGuiElement(index) {
  const el = Number.isInteger(index) && index >= 0 && index < guiState.elements.length
    ? guiState.elements[index] : null;
  guiState.selectedIndex = el ? index : -1;
  showGuiElementDetail(el);
}

// The highlight box is positioned in zoomed pixels, so re-apply it after a
// zoom change while keeping the current dropdown selection.
function refreshGuiSelection() {
  showGuiElementDetail(guiState.selectedIndex >= 0
    ? guiState.elements[guiState.selectedIndex] : null);
}

function showGuiElementDetail(el) {
  const box = $("guiSelectBox");
  if (!el) { box.hidden = true; $("guiInfo").innerHTML = GUI_INFO_PROMPT; return; }
  const r = Array.isArray(el.rect) ? el.rect : [0, 0, 0, 0];
  const z = guiState.zoom;
  box.style.left = `${r[0] * z}px`;
  box.style.top = `${r[1] * z}px`;
  box.style.width = `${r[2] * z}px`;
  box.style.height = `${r[3] * z}px`;
  box.hidden = false;
  const row = (k, v) => `<div class="gi-row"><span class="gi-key">${k}</span><span class="gi-val">${v}</span></div>`;
  const a = el.art;
  let html = `<div class="gi-title">${el.label || el.id || "元素"}</div>`;
  html += row("id", el.id || "-") + row("类型", el.type || "-") + row("矩形", `[${r.join(", ")}]`);
  if (el.panel) html += row("分区", `${el.panel} ${JSON.stringify(el.panel_rect)}`);
  html += row("z", el.z ?? "-");
  if (a) html += row("美术", `${a.name || "-"}<br>${a.format || "-"} · w=${a.width ?? "-"} · frame ${a.frame ?? "-"}/${a.frames ?? "-"}`) +
                 row("调色板", a.palette || "-") + row("解码", el.decode_status || "-");
  if (el.grid) html += row("网格", `${el.grid.cols}×${el.grid.rows} · cell ${el.grid.cell} · step ${el.grid.step}`);
  if (el.text) html += row("文字", guiExampleText(el.text)) +
                       row("字号/色", `${el.font_size} / ${el.color || "默认"} / ${el.align || "left"}`);
  html += row("源码", el.source_ref || "-");
  if (el.text && /\{/.test(el.text)) html += `<div class="gi-note">占位符已替换为示意值（非运行时数据）；文字为系统字体近似，非游戏原版位图字体。</div>`;
  else html += `<div class="gi-note">文字为系统字体近似，非游戏原版位图字体。</div>`;
  $("guiInfo").innerHTML = html;
}

function wireGuiControls() {
  // Initialise the element dropdown with its placeholder even before any
  // screen has loaded (no archives / not connected yet -> still shows a hint).
  clearGuiElementState();
  $("guiLoadBtn").addEventListener("click", () => loadGuiScreen());
  $("guiSel").addEventListener("change", () => loadGuiScreen());
  $("guiElementSel").addEventListener("change", (e) => {
    selectGuiElement(e.target.value === "" ? -1 : parseInt(e.target.value, 10));
  });
  $("guiZoom").addEventListener("change", (e) => {
    guiState.zoom = Math.max(1, Math.min(2, parseInt(e.target.value, 10) || 1));
    applyGuiTransform();
    refreshGuiSelection();
  });
  $("guiOverlayToggle").addEventListener("change", (e) => {
    $("guiOverlay").hidden = !e.target.checked;
  });
  $("guiNumberToggle").addEventListener("change", () => {
    // number tags are baked into the backend overlay PNG -> rebuild.
    if (guiState.loaded) loadGuiScreen();
  });
  $("guiTextToggle").addEventListener("change", () => {
    // text layer is drawn client-side; just redraw, no backend call.
    drawGuiText($("guiText"), guiState.elements);
  });
}

function wireControls() {
  $("connectBtn").addEventListener("click", async () => {
    try {
      await client.connect($("wsUrl").value.trim());
      await loadCatalog();
      // If the user switched to the scene/GUI view before connecting, nothing
      // could load then; now that archives are known, draw it.
      if (isSceneMode() && !sceneState.loaded) await showSceneView();
      if (isGuiMode() && !guiState.loaded) await showGuiView();
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
      const combat = e.target.value === "combat";
      const scene = e.target.value === "scene";
      const gui = e.target.value === "gui";
      const viewOwned = scene || gui;
      stopPlay();
      $("catalogPane").hidden = character || monster || combat || viewOwned;
      $("charPane").hidden = !character;
      $("monsterPane").hidden = !monster;
      $("combatPane").hidden = !combat;
      $("combatDetailPane").hidden = !combat;
      $("scenePane").hidden = !scene;
      $("guiPane").hidden = !gui;
      // Scene / GUI mode swaps the shared canvas for its own PNG viewport.
      $("canvasWrap").hidden = viewOwned;
      $("sceneView").hidden = !scene;
      $("guiView").hidden = !gui;
      $("overlaysPanel").hidden = viewOwned;
      // Combat uses a fixed 8-way scene direction; other modes use the sprite's
      // own direction count when the next sprite is decoded.
      if (combat) syncCombatDirControl();
      else if (!viewOwned) {
        $("directions").disabled = false;
        $("dir").max = String(Math.max(0, currentDirections - 1));
      }
      // Character / monster / combat all want level palettes (correct colours).
      applyPaletteFilter(character || monster || combat);
      if (character) showCharacterSprite();
      if (monster) showMonsterView();
      if (combat) showCombatView();
      if (scene) showSceneView();
      if (gui) showGuiView();
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

  // Frame slider (frame within the current direction / combat stage).
  $("frame").addEventListener("input", (e) => {
    if (isViewOwnedMode()) return;
    stopPlay();
    const v = Number(e.target.value);
    if (isCombatMode()) renderCombatStage(combatStage, v);
    else showFrame(currentDir, v);
  });
  // Direction slider. In combat mode it rotates the whole encounter around the
  // caster: both actors are re-placed and re-oriented, but playback never
  // changes the direction (the loop only advances frames within one stage).
  $("dir").addEventListener("input", (e) => {
    if (isViewOwnedMode()) return;
    if (isCombatMode()) {
      combatDir = Math.max(0, Math.min(7, Number(e.target.value) || 0));
      renderCombatStage(combatStage, Math.min(combatFrame, stageFrameCount(combatStage) - 1));
      return;
    }
    stopPlay();
    showFrame(Number(e.target.value), currentFin);
  });
  // Directions selector (re-splits the flat frame list).
  $("directions").addEventListener("change", (e) => {
    if (isCombatMode() || isViewOwnedMode()) return;
    stopPlay();
    setDirections(Number(e.target.value));
    currentFin = Math.min(currentFin, framesPerDir - 1);
    $("frame").max = String(framesPerDir - 1);
    $("frame").value = String(currentFin);
    showFrame(currentDir, currentFin);
  });
  $("playBtn").addEventListener("click", () => (isPlaying ? stopPlay() : startPlay()));

  // Display zoom (1..8). Baked into the canvas bitmap at draw time (see viewer.js
  // drawComposite/drawScene: canvas pixels = native × zoom, context scaled with
  // nearest-neighbour), so sprite size AND scene spacing both grow with zoom.
  $("zoom").addEventListener("input", (e) => {
    if (isViewOwnedMode()) return;
    zoom = Math.max(1, Math.min(8, parseInt(e.target.value, 10) || 1));
    $("zoomNo").textContent = zoom + "×";
    if (isCombatMode()) renderCombatStage(combatStage, combatFrame);
    else redrawCurrent();
  });

  // Re-decode the current sprite when the user changes the palette.
  $("palette").addEventListener("change", () => {
    if (isViewOwnedMode()) return;
    if (isCombatMode()) { combatCache.clear(); buildCombatScene(); return; }
    if (lastSprite) onSelectSprite(lastSprite);
  });

  // TRN toggle / preset: re-decode so the colour change takes effect.
  $("applyTrn").addEventListener("change", () => {
    if (isViewOwnedMode()) return;
    if (isCombatMode()) { combatCache.clear(); buildCombatScene(); return; }
    if (lastSprite) onSelectSprite(lastSprite);
  });
  $("trnPreset").addEventListener("change", () => {
    if (isCombatMode() || isViewOwnedMode()) return;
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

  // Effect-overlay panel: collapsed by default; expand refreshes the quick-add
  // buttons (mode-aware) and the generic picker.
  $("overlayToggle").addEventListener("click", () => {
    const body = $("overlaysBody");
    const open = body.hidden;
    body.hidden = !open;
    $("overlayToggle").setAttribute("aria-expanded", String(open));
    $("overlayToggle").textContent = open ? "收起" : "展开";
    if (open) {
      populateOverlayQuick();
      populateOverlayPick();
    }
  });
  $("overlayAddBtn").addEventListener("click", () => {
    const name = $("overlayPick").value;
    if (!name) return;
    addOverlay(name);
  });

  // Skill / combat viewer controls.
  $("combatSource").addEventListener("change", () => {
    populateCombatSpells();
    selectCombatEntry();
    buildCombatScene();
  });
  $("combatSpell").addEventListener("change", () => {
    selectCombatEntry();
    buildCombatScene();
  });
  $("combatClass").addEventListener("change", () => buildCombatScene());
  $("combatArchive").addEventListener("change", () => {
    combatCache.clear();
    buildCombatScene();
  });
  $("combatMonster").addEventListener("change", () => {
    fillCombatMonsterAnims();
    buildCombatScene();
  });
  $("combatMonsterAnim").addEventListener("change", () => buildCombatScene());
  $("stagePrev").addEventListener("click", () => stepCombatStage(-1));
  $("stageNext").addEventListener("click", () => stepCombatStage(1));
  $("combatLoop").addEventListener("change", () => {
    if (isPlaying) { stopPlay(); startCombatPlay(); }
  });

  wireSceneControls();
  wireGuiControls();
}

wireControls();
