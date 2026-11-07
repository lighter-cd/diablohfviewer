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
import { drawFrame, fitCanvas, drawComposite, drawScene } from "./viewer.js";

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

// Skill / combat viewer: stage-ordered full-cast timeline (player + monster +
// effect layers in one scene). spell.list supplies the catalog.
let spellCatalog = null;       // spell.list result
let combatEntry = null;        // currently selected spell / monster attack
let combatStage = 0;           // index into combatEntry.stages
let combatFrame = 0;           // frame index within that stage
let combatCache = new Map();   // "name|width|trn" -> decoded frames[]
let combatActors = { playerStand: [], playerCast: [], monsterBase: [], monsterTrn: [] };
let combatStageFrames = [];    // per stage: [{ frames, anchor, role }]
let combatTimeline = [];       // flattened [{ stageIdx, frameIdx }]
let combatPlayIdx = 0;

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
  if (isCombatMode()) return;   // combat mode owns the canvas via drawScene
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

  drawComposite($("canvas"), f.rgba_base64, f.width, f.height, ovRender);
  fitCanvas($("canvas"), 384);
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
  return $("combatSource").value === "monster"
    ? (spellCatalog.monster_attacks || [])
    : (spellCatalog.spells || []);
}

function populateCombatSpells() {
  const sel = $("combatSpell");
  sel.innerHTML = "";
  for (const e of combatList()) {
    const o = document.createElement("option");
    o.value = e.id;
    o.textContent = e.name + (e.monster_ai ? `  (${e.monster_ai})` : "");
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

async function decodeCombatFrames(name, width, trn) {
  const key = name + "|" + (width === "auto" ? "auto" : width) + "|" + (trn || "");
  if (combatCache.has(key)) return combatCache.get(key);
  const args = buildDecodeArgs(
    { name, archive_path: combatArchiveFor() }, name, { width, trn });
  const res = await client.callTool("sprite.decode", args);
  const frames = (res && res.frames) || [];
  combatCache.set(key, frames);
  return frames;
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

// A sprite layer can span several numbered .cl2 files; flatten them into one
// frame sequence so the stage timeline can play straight through.
async function decodeLayerFrames(layer) {
  const key = "layer|" + (layer.files || []).join("|") + "|" + layer.width;
  if (combatCache.has(key)) return combatCache.get(key);
  const frames = [];
  for (const f of layer.files || [])
    frames.push(...await decodeCombatFrames(f, layer.width));
  combatCache.set(key, frames);
  return frames;
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
    if (ce) {
      const action = combatEntry.cast_action || "qm";
      const combo = combatCombo(ce, action);
      combatActors.playerStand =
        await decodeCombatFrames(plrgfxPath(cp, ce, combo, "st"), "auto");
      try {
        combatActors.playerCast =
          await decodeCombatFrames(plrgfxPath(cp, ce, combo, action), "auto");
      } catch (e) {
        combatActors.playerCast = [];   // fall back to standing if unavailable
      }
      if (!combatActors.playerCast.length)
        combatActors.playerCast = combatActors.playerStand;
    }

    const mName = $("combatMonsterAnim").value;
    const trn = combatDebuffTrn();
    combatActors.monsterBase = mName ? await decodeCombatFrames(mName, "auto") : [];
    combatActors.monsterTrn = (mName && trn)
      ? await decodeCombatFrames(mName, "auto", trn) : [];

    combatStageFrames = [];
    for (const st of combatEntry.stages || []) {
      const layers = [];
      for (const l of st.layers || []) {
        if (l.kind !== "sprite") continue;
        layers.push({ frames: await decodeLayerFrames(l), anchor: l.anchor, role: l.role });
      }
      combatStageFrames.push(layers);
    }
    buildCombatTimeline();
    hideError();
    renderCombatStage(0, 0);
  } catch (e) {
    showError("战斗场景加载失败：" + (e.message || e));
  }
}

function stageFrameCount(si) {
  const st = ((combatEntry && combatEntry.stages) || [])[si];
  if (!st) return 1;
  if (st.id === "cast")
    return Math.max(1, (combatActors.playerCast || []).length ||
                       (combatActors.playerStand || []).length || 8);
  let n = 1;
  for (const l of combatStageFrames[si] || []) n = Math.max(n, l.frames.length);
  return n;
}

function buildCombatTimeline() {
  combatTimeline = [];
  ((combatEntry && combatEntry.stages) || []).forEach((st, si) => {
    const n = stageFrameCount(si);
    for (let f = 0; f < n; f++) combatTimeline.push({ stageIdx: si, frameIdx: f });
  });
  combatPlayIdx = 0;
}

function combatActorFrame(frames, fi) {
  if (!frames || !frames.length) return null;
  return frames[fi % frames.length];
}

function renderCombatStage(si, fi) {
  if (!combatEntry) return;
  const stages = combatEntry.stages || [];
  if (!stages.length) return;
  si = Math.max(0, Math.min(si, stages.length - 1));
  const st = stages[si];
  combatStage = si;
  combatFrame = fi;
  const actors = [];

  // Player (left): casting animation on the cast stage, standing otherwise.
  const pf = combatActorFrame(
    st.id === "cast" ? combatActors.playerCast : combatActors.playerStand, fi);
  if (pf) actors.push({ rgba: pf.rgba_base64, w: pf.width, h: pf.height, x: 40, y: 72, z: 1 });

  // Monster (right): TRN tint from the debuff stage onwards.
  const debuffIdx = stages.findIndex((s) => s.id === "debuff");
  const tinted = debuffIdx >= 0 && si >= debuffIdx && (combatActors.monsterTrn || []).length;
  const mf = combatActorFrame(
    tinted ? combatActors.monsterTrn : combatActors.monsterBase, fi);
  if (mf) actors.push({ rgba: mf.rgba_base64, w: mf.width, h: mf.height, x: 300, y: 72, z: 1 });

  // Effect layers (projectile / impact / debuff sprites).
  let k = 0;
  for (const l of combatStageFrames[si] || []) {
    const f = combatActorFrame(l.frames, fi);
    if (f) {
      let x = 180 + (k % 2) * 40;
      if (l.anchor === "monster") x = 296;
      else if (l.anchor === "player") x = 40;
      actors.push({ rgba: f.rgba_base64, w: f.width, h: f.height,
                    x, y: 60 + (k % 3) * 8, z: 2 });
    }
    k++;
  }

  drawScene($("canvas"), 480, 256, actors);
  fitCanvas($("canvas"), 480);
  $("dirNo").textContent = String(si);
  $("frameNo").textContent = `${st.label} · 帧 ${fi} / ${Math.max(0, stageFrameCount(si) - 1)}`;
  $("stageNo").textContent = `${si + 1}/${stages.length} ${st.label}`;
  $("frame").max = String(Math.max(0, stageFrameCount(si) - 1));
  $("frame").value = String(Math.min(fi, Math.max(0, stageFrameCount(si) - 1)));
  $("currentName").textContent = `${combatEntry.name} — ${st.label}`;
  setMeta(`技能/战斗查看器  ${combatEntry.name}  ·  ${st.label}  ·  ${st.layers.length} 层`);
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
      const combat = e.target.value === "combat";
      stopPlay();
      $("catalogPane").hidden = character || monster || combat;
      $("charPane").hidden = !character;
      $("monsterPane").hidden = !monster;
      $("combatPane").hidden = !combat;
      // Character / monster / combat all want level palettes (correct colours).
      applyPaletteFilter(character || monster || combat);
      if (character) showCharacterSprite();
      if (monster) showMonsterView();
      if (combat) showCombatView();
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
    stopPlay();
    const v = Number(e.target.value);
    if (isCombatMode()) renderCombatStage(combatStage, v);
    else showFrame(currentDir, v);
  });
  // Direction slider (combat mode uses the stage stepper instead).
  $("dir").addEventListener("input", (e) => {
    if (isCombatMode()) return;
    stopPlay();
    showFrame(Number(e.target.value), currentFin);
  });
  // Directions selector (re-splits the flat frame list).
  $("directions").addEventListener("change", (e) => {
    if (isCombatMode()) return;
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
    if (isCombatMode()) { combatCache.clear(); buildCombatScene(); return; }
    if (lastSprite) onSelectSprite(lastSprite);
  });

  // TRN toggle / preset: re-decode so the colour change takes effect.
  $("applyTrn").addEventListener("change", () => {
    if (isCombatMode()) { combatCache.clear(); buildCombatScene(); return; }
    if (lastSprite) onSelectSprite(lastSprite);
  });
  $("trnPreset").addEventListener("change", () => {
    if (isCombatMode()) return;
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
}

wireControls();
