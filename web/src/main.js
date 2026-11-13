// web/src/main.js
//
// Glue: connect to the mcp-server WS bridge, load the sprite catalog via
// sprite.list, and decode sprites via sprite.decode for Canvas rendering.
//
// Browse modes; Catalog / Entity / Combat share the canvas / palette / direction
// / playback controls, while Scene and GUI each own a dedicated PNG viewport:
//   - Catalog (目录浏览): pick any sprite from the recovered-name tree.
//   - Entity (实体查看器): merged player / monster / NPC viewer. A left
//     #entityType dropdown switches between 玩家角色 (Character), 怪物 (Monster)
//     and NPC (Towner); each shows only its own parameter panel and all three
//     share the same sprite.decode + 8-direction playback.
//   - Scene (场景查看器): scene.list / scene.build complete level renders.
//   - GUI (界面布局查看器, 6th): ui.list / ui.build reconstruct DiabloUI
//     screens from real MPQ art + an approximate-font text layer.
//
// Player CL2 frames are stored direction-major: 8 directions x N frames, so
// frameIndex = dir * framesPerDir + frameInDir (framesPerDir = total/8).

import { McpClient } from "./client.js?v=20261113d";
import { buildTree, renderCatalog, setAllExpanded } from "./catalog.js?v=20261113d";
import { drawComposite, drawScene } from "./viewer.js?v=20261113d";
import { t, tOpt, getLang, initI18n, applyI18n, wireLangSelectors, onLangChange } from "./i18n.js?v=20261113d";
import { renderScene } from "./scene-render.js?v=20261113d";
import { buildLightMap } from "./scene-light.js?v=20261113d";

// Load the string tables BEFORE anything else runs. Module scripts are deferred,
// so the whole DOM is already parsed here. onLangChange (registered at the
// bottom) re-renders every runtime-built string without a page reload.
await initI18n();
applyI18n();
wireLangSelectors();

const $ = (id) => document.getElementById(id);

// Separator after a key in the info/detail rows: full-width colon in Chinese.
const SEP = () => (getLang() === "en" ? ": " : "：");

// Escape untrusted backend data before it is inserted via innerHTML.
function esc(v) {
  return String(v == null ? "" : v)
    .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
}

// Backend-owned titles/descriptions are Chinese data, not UI chrome. Override the
// known ids from the config files (data.*) so English mode reads consistently;
// unknown ids fall back to whatever the backend supplied.
function dataTitle(group, id, backendTitle) {
  return tOpt(`data.${group}.${id}`, backendTitle);
}
function stageLabel(st) {
  if (!st) return "";
  const id = st.id || "";
  const suffix = id ? id.charAt(0).toUpperCase() + id.slice(1) : "";
  return tOpt(`combat.stage${suffix}`, st.label || id);
}

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
function optLabel(code, map, group) {
  // Prefer the localized name from the config files; the English maps below are
  // the fallback for locales that do not define an enum entry.
  const n = (group && tOpt(`enum.${group}.${code}`, null)) || map[code];
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

let connected = false;
client.onStatus = (isConnected) => {
  connected = isConnected;
  const s = $("status");
  s.textContent = isConnected ? t("conn.connected") : t("conn.disconnected");
  s.className = "status " + (isConnected ? "ok" : "bad");
};

// #meta was folded into the top "当前资源" line. Every status line is stored as
// a render closure so a language switch can re-translate it (and the error
// banner) without losing the currently displayed resource / error.
let metaRender = null;
let errorRender = null;

function setMetaRender(fn, isError = false) {
  metaRender = fn;
  const m = $("currentName");
  m.className = isError ? "current-name err" : "current-name";
  m.textContent = fn();
}
function setMetaT(key, vars, isError = false) {
  setMetaRender(() => t(key, vars), isError);
}
function setMeta(text, isError = false) {
  if (!text) return;                 // clear-only call: keep the current line
  setMetaRender(() => text, isError);
}
function renderMeta() {
  if (metaRender) $("currentName").textContent = metaRender();
}

function showErrorRender(fn) {
  errorRender = fn;
  const b = $("errorBanner");
  b.textContent = fn();
  b.hidden = false;
}
function showErrorT(key, vars) {
  showErrorRender(() => t(key, vars));
}
function showError(msg) {
  showErrorRender(() => msg);
}
function hideError() {
  errorRender = null;
  $("errorBanner").hidden = true;
}
function renderError() {
  if (errorRender) $("errorBanner").textContent = errorRender();
}

// ---- Global MPQ entry (P1) --------------------------------------------------
//
// The user points at their own legitimate Hellfire / Diablo MPQ directory. This
// is the SINGLE shared entry for all five browse modes. The directory is persisted
// to localStorage. The config UI lives in a <dialog> opened by the "mpq配置"
// button beside the connection status; that button is red when unconfigured and
// green when configured. Behaviour:
//   - Not configured  -> red button, DO NOT auto-read the MPQ. The viewer waits
//                         for the user to provide it.
//   - Configured      -> green button; on the next frontend entry, auto-connect +
//                         auto-load the catalog instead of waiting for a click.
// The backend itself reads a pre-generated recovered.json (absolute MPQ paths),
// so the directory here is the user-facing anchor that gates auto-loading.
const MPQ_DIR_KEY = "dhfc.mpqDir";

function getMpqDir() {
  try { return (localStorage.getItem(MPQ_DIR_KEY) || "").trim(); }
  catch { return ""; }
}

function setMpqButton(configured) {
  const b = $("mpqConfigBtn");
  b.className = "mpq-config-btn " + (configured ? "ok" : "bad");
  b.title = configured ? t("mpq.configuredTip") : t("mpq.unconfiguredTip");
}
function setMpqStatusConfigured(dir) {
  const s = $("mpqStatus");
  s.textContent = t("mpq.configured", { dir });
  s.className = "mpq-status ok";
  setMpqButton(true);
}
function setMpqStatusUnconfigured() {
  const s = $("mpqStatus");
  s.textContent = t("mpq.unconfigured");
  s.className = "mpq-status bad";
  setMpqButton(false);
}
function setMpqPickMsg(text, kind) {
  const m = $("mpqPickMsg");
  m.textContent = text || "";
  m.className = "mpq-pick-msg" + (kind ? " " + kind : "");
}

// Reflect the persisted directory into the input + dialog badge + button colour.
// Called on load.
function reflectMpqConfig() {
  const dir = getMpqDir();
  if (dir) {
    $("mpqDir").value = dir;
    setMpqStatusConfigured(dir);
  } else {
    $("mpqDir").value = "";
    setMpqStatusUnconfigured();
  }
}

// Save the directory the user typed / pasted: persist it, update the badge + button
// colour, and (only if it changed) connect + load the catalog so the MPQ is read
// right away.
async function saveMpqDir() {
  const dir = ($("mpqDir").value || "").trim();
  if (!dir) {
    try { localStorage.removeItem(MPQ_DIR_KEY); } catch {}
    $("mpqDir").value = "";
    setMpqStatusUnconfigured();
    return;
  }
  const prev = getMpqDir();
  try { localStorage.setItem(MPQ_DIR_KEY, dir); } catch {}
  setMpqStatusConfigured(dir);
  if (dir === prev) return;
  setMetaT("mpq.saved", { dir });
  if (!client.ws || client.ws.readyState !== WebSocket.OPEN) {
    try {
      await client.connect($("wsUrl").value.trim());
      await loadCatalog();
      if (isSceneMode() && !sceneState.loaded) await showSceneView();
      if (isGuiMode() && !guiState.loaded) await showGuiView();
    } catch (e) {
      setMetaT("mpq.autoLoadFailed", { error: e.message || e }, true);
    }
  }
}

// Wire the "mpq配置" button + its dialog. Opening always re-syncs the input from
// the saved value, so a previously cancelled edit is never shown as if saved.
// Saving persists the directory (and, when changed, triggers auto-connect/load
// inside saveMpqDir) then closes the dialog immediately; Esc / the header × /
// 取消 / a backdrop click all close without saving.
function wireMpqConfig() {
  const modal = $("mpqModal");
  const close = () => { if (modal.open) modal.close(); };

  $("mpqConfigBtn").addEventListener("click", () => {
    $("mpqDir").value = getMpqDir();
    setMpqPickMsg("");
    modal.showModal();
    $("mpqDir").focus();
  });
  $("mpqClose").addEventListener("click", close);
  $("mpqCancel").addEventListener("click", close);
  modal.addEventListener("click", (e) => {
    const r = modal.getBoundingClientRect();
    if (e.clientX < r.left || e.clientX > r.right ||
        e.clientY < r.top || e.clientY > r.bottom) close();
  });

  const save = () => {
    saveMpqDir();   // fire-and-forget: handles its own errors, may auto-connect/load
    close();
  };
  $("mpqSave").addEventListener("click", save);
  $("mpqDir").addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); save(); }
  });
}

// On frontend entry: if a directory is already configured, automatically connect
// and load the catalog (auto-read the MPQ to fetch the directory listing). If not,
// keep the button red and deliberately do NOT read the MPQ — the user must fill
// it in first (via the "mpq配置" dialog). Manual Connect works in either case.
async function initMpqEntry() {
  const dir = getMpqDir();
  if (!dir) {
    setMpqStatusUnconfigured();
    return;
  }
  $("mpqDir").value = dir;
  setMpqStatusConfigured(dir);
  setMetaT("mpq.autoLoading");
  try {
    await client.connect($("wsUrl").value.trim());
    await loadCatalog();
    if (isSceneMode() && !sceneState.loaded) await showSceneView();
    if (isGuiMode() && !guiState.loaded) await showGuiView();
  } catch (e) {
    setMetaT("mpq.autoLoadFailed", { error: e.message || e }, true);
  }
}

function basename(p) {
  const i = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"));
  return i >= 0 ? p.slice(i + 1) : p;
}
function extOf(p) {
  const i = p.lastIndexOf(".");
  return i >= 0 ? p.slice(i + 1).toLowerCase() : "";
}

function isEntityMode() {
  const r = document.querySelector('input[name="mode"]:checked');
  return !!r && r.value === "entity";
}

// The three entity sub-types all live inside the single "entity" browse mode;
// which one is active is decided by the #entityType dropdown on the left.
function isCharMode() {
  return isEntityMode() && $("entityType").value === "character";
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
  setMetaT("status.loadingCatalog");
  // P1: tell the backend which directory to read the MPQ files from. The backend
  // redirects every archive path in recovered.json into this directory, so the
  // viewer reads the user's own legitimate copy — nothing is uploaded anywhere.
  const mpqDir = getMpqDir();
  const listArgs = mpqDir ? { mpq_dir: mpqDir } : {};
  try {
    const res = await client.callTool("sprite.list", listArgs);
    const sprites = (res && res.sprites) || [];
    lastSprites = sprites.slice();
    // Also pull the UI layout's referenced art (ui_art\*.pcx, data\*.clx/.cel,
    // ctrlpan\spelicon.cel, ...) so the Catalog browser can show the same art the
    // GUI layout viewer composites, with frame info for playback.
    try {
      const uiRes = await client.callTool("sprite.list", { ...listArgs, kind: "ui" });
      const uiSprites = (uiRes && uiRes.sprites) || [];
      for (const s of uiSprites) if (!lastSprites.some((x) => x.name === s.name))
        lastSprites.push(s);
    } catch (e) {
      console.warn("sprite.list(kind:ui) failed:", e);
    }
    renderCurrentCatalog();
    setMetaT("status.catalogLoaded", { count: lastSprites.length });
    // Build the FULL MPQ stack. sprite.list reports every archive from
    // recovered.json, including ones holding no CL2/CEL sprites (notably
    // devilutionx.mpq, which owns the .clx UI art + ui_art\diablo.pal). Deriving
    // the stack only from sprite entries made ui.build / scene.build miss those
    // archives, so char panel / store / dialog backgrounds reported `missing`
    // and their CLX `decode_error`. Union with per-sprite paths for older
    // backends that don't return `archives` yet.
    allArchives = [];
    const norm = (p) => (p || "").trim().replace(/\\/g, "/");
    for (const a of (res && res.archives) || []) {
      const p = norm(typeof a === "string" ? a : (a && a.path));
      if (p && !allArchives.includes(p)) allArchives.push(p);
    }
    for (const s of sprites) {
      const p = norm(s.archive_path);
      if (p && !allArchives.includes(p)) allArchives.push(p);
    }
    await loadPalettes();
    await loadCharacterData();   // build the class/action/armor/weapon index
    await loadSpellList();       // stage-ordered spell / monster-attack catalog
  } catch (e) {
    setMetaT("error.spriteList", { error: e.message }, true);
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
      addPaletteOption(t("stage.noPalette"), "");
      return;
    }
    applyPaletteFilter(isCharMode());
  } catch (e) {
    addPaletteOption(t("stage.paletteFailed"), "");
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
    opt.textContent = basename(p.name) +
      (p.isLevel ? t("stage.levelPalette") : t("stage.otherPalette"));
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
  for (const code of [...e.types].sort())
    addOption(sel, code, optLabel(code, TYPE_NAMES, "type"));
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
  fillSelect($("charArmor"), [...armors].sort(), ARMOR_NAMES, "armor");
  fillSelect($("charWeapon"), [...weapons].sort(), WEAPON_NAMES, "weapon");
}

// Rebuild the character dropdowns on a language switch, keeping the current
// selections (option VALUES are data, only their labels are translated).
function refreshCharOptions() {
  const act = $("charAction").value;
  const arm = $("charArmor").value;
  const wep = $("charWeapon").value;
  fillCharActions();
  if ([...$("charAction").options].some((o) => o.value === act)) $("charAction").value = act;
  fillCharArmorWeapon();
  if ([...$("charArmor").options].some((o) => o.value === arm)) $("charArmor").value = arm;
  if ([...$("charWeapon").options].some((o) => o.value === wep)) $("charWeapon").value = wep;
}

function addOption(sel, value, text) {
  const o = document.createElement("option");
  o.value = value;
  o.textContent = text;
  sel.appendChild(o);
}
function fillSelect(sel, codes, names, group) {
  sel.innerHTML = "";
  for (const c of codes) addOption(sel, c, optLabel(c, names, group));
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

// Fill the MPQ <select> in both the monster and NPC panels (they are separate
// elements so each entity type owns its own control set).
function populateMonsterArchive() {
  for (const id of ["monsterArchive", "npcArchive"]) {
    const sel = $(id);
    const prev = sel.value;
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
      o.textContent = t("common.noMpq");
      sel.appendChild(o);
    }
    if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
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
    o.textContent = `${s.anim || s.type} (${t("entity.framesPerDir", { n: s.frames_per_dir || "?" })})`;
    o.title = s.name;
    sel.appendChild(o);
  }
}

// Build the NPC (towner) dropdown from sprite.list(family:"towners"). The
// fetched list is cached so a language switch can rebuild the labels without
// another backend round-trip.
let npcSprites = [];
function renderNpcOptions() {
  const sel = $("npcSel");
  const prev = sel.value;
  sel.innerHTML = "";
  for (const s of npcSprites) {
    const o = document.createElement("option");
    o.value = s.name;
    o.textContent = `${s.class || basename(s.name)}  (${basename(s.name)})`;
    o.title = s.name;
    sel.appendChild(o);
  }
  if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
}
async function populateNPCs() {
  try {
    const res = await client.callTool("sprite.list", { family: "towners" });
    npcSprites = (res && res.sprites) || [];
  } catch (e) {
    console.warn("towner list failed:", e.message);
    npcSprites = [];
  }
  renderNpcOptions();
}

// Decode the selected monster (family monsters, directions from backend).
function showMonsterSprite() {
  const archive = $("monsterArchive").value;
  const name = $("monsterAnim").value;
  if (!name) return;
  onSelectSprite({ name, archive_path: archive, family: "monsters", class: $("monsterSel").value });
}

// Decode the selected NPC (family towners, single direction). NPC owns its own
// #npcArchive select, distinct from the monster panel's #monsterArchive.
function showNpcSprite() {
  const archive = $("npcArchive").value;
  const name = $("npcSel").value;
  if (!name) return;
  onSelectSprite({ name, archive_path: archive, family: "towners", class: "npc" });
}

// Switch the entity viewer to the selected type. The three entity types each own
// ONE independent parameter panel (panel-character / panel-monster / panel-npc);
// we show exactly the matching one and hide the other two, so a type's controls
// are never mixed with another's. The first decode for that type is triggered too.
async function syncEntityType() {
  const t = $("entityType").value;
  $("panel-character").hidden = (t !== "character");
  $("panel-monster").hidden = (t !== "monster");
  $("panel-npc").hidden = (t !== "npc");
  if (!allArchives.length) {
    showErrorT("error.noArchives");
    return;
  }
  if (t === "character") {
    showCharacterSprite();
  } else if (t === "monster") {
    populateMonsterArchive();
    await populateMonsters();
    showMonsterSprite();
  } else { // npc
    populateMonsterArchive();
    await populateNPCs();
    showNpcSprite();
  }
}

// ---- Decode + 8-direction playback -----------------------------------------

// Build the sprite.decode argument object for `sprite` (optionally overriding the
// sprite name to decode a variant of the same base). Reuses the current palette +
// TRN settings so the decoded frame matches the viewer's current color mode.
function buildDecodeArgs(sprite, nameOverride, opts = {}) {
  // recovered.json stores archive paths with a mix of "/" and "\". Normalize
  // them: if the same MPQ reaches the backend under two spellings, StormLib is
  // asked to open it twice and fails ("cannot open archive").
  const norm = (p) => (p || "").trim().replace(/\\/g, "/");
  const archive = norm(sprite.archive_path);
  // UI art (family "ui") carries its own authoritative palette (embedded for
  // .pcx, "ui_art\\diablo.pal" for menu/dialog .clx, "levels\\towndata\\town.pal"
  // for in-game .cel/.clx). Prefer it over the global palette dropdown so the
  // Catalog browser can render UI leaves without forcing a manual palette pick.
  // A PCX leaf carries palette:"embedded" — its colours live INSIDE the file, so
  // we must NOT pass palette_name (which resolvePalette would apply BEFORE the
  // embedded one, recolouring the PCX with a game palette and rendering garbage).
  // For embedded PCX we omit palette_name entirely and let sprite.decode fall back
  // to the PCX's own embedded palette; the global dropdown is then ignored.
  const isEmbeddedPcx = !!(sprite && sprite.palette === "embedded");
  const ownPalette = (!isEmbeddedPcx && sprite && sprite.palette)
    ? sprite.palette : "";
  const palette = ownPalette || $("palette").value;
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
    // spell.list; width:"auto" only works for un-numbered sprite paths. For
    // catalog entries that already carry a known width (e.g. UI art CEL with an
    // explicit frame width), prefer that over "auto" so decode succeeds.
    width: opts.width !== undefined ? opts.width
          : (sprite && sprite.width ? sprite.width : "auto"),
  };
  if (!isEmbeddedPcx) args.palette_name = palette;
  // Per-frame widths for CELs whose frames differ in width (data\inv\objcurs*.cel
  // inventory icons). sprite.list does not ship the 179/61-wide arrays (too big
  // for the WS payload); the backend resolves them from a built-in table when
  // width:"auto" is sent. This passthrough covers direct calls / overlay layers.
  const widths = opts.widths !== undefined ? opts.widths
               : (sprite && Array.isArray(sprite.widths) ? sprite.widths : undefined);
  if (widths !== undefined) args.widths = widths;
  // An explicit TRN (combat debuff tint on the target) wins over the global
  // toggle. Otherwise the toggle uses the class TRN or a chosen preset.
  if (opts.trn) {
    args.apply_trn = true;
    args.trn_name = opts.trn;
  } else {
    // The TRN preset select alone controls the remap: "无" (empty) disables it,
    // any other value enables it. The old "应用 TRN" checkbox is gone.
    const preset = $("trnPreset").value;
    if (preset) {
      args.apply_trn = true;
      if (preset === "infra") args.trn_name = "plrgfx\\infra.trn";
      else if (preset === "stone") args.trn_name = "plrgfx\\stone.trn";
      // preset "class" -> let the backend compute plrgfx\trn<class>.trn
    }
  }
  return args;
}

async function onSelectSprite(sprite) {
  lastSprite = sprite;
  hideError();
  $("currentName").textContent = sprite.name;

  // Level tileset CELs (levels\l<d>data\l<d>.cel / nlevels\...) are NOT frame
  // sprites — they need the dedicated Level Tile Viewer (level.inspect), which
  // assembles TIL→MIN→CEL with the right palette variant. Route the click there
  // instead of sprite.decode (which would fail: "cel requires a width").
  if (sprite.family === "level") {
    if (sprite.tileset) {
      const url = `level.html?tileset=${encodeURIComponent(sprite.tileset)}`;
      window.open(url, "_blank", "noopener");
      setMetaT("status.levelOpened", { name: sprite.name });
    } else {
      showErrorT("error.noTileset", { name: sprite.name });
    }
    stopPlay();
    return;
  }

  $("combatInfo").hidden = true; $("combatInfo").textContent = "";

  const ext = extOf(sprite.name);
  if (!DECODABLE.has(ext)) {
    showErrorT("error.unsupportedFormat", { ext: ext || "?" });
    stopPlay();
    return;
  }

  const archive = (sprite.archive_path || "").trim();
  if (!archive) {
    showErrorT("error.noArchivePath");
    stopPlay();
    return;
  }
  const palette = $("palette").value;
  const ownPalette = (sprite.palette && sprite.palette !== "embedded");
  if (!ownPalette && !palette) {
    showErrorT("error.noPaletteSelected");
    stopPlay();
    return;
  }

  setMetaT("status.decoding", { name: sprite.name });
  stopPlay();
  try {
    const args = buildDecodeArgs(sprite);
    const res = await client.callTool("sprite.decode", args);
    if (!res || !Array.isArray(res.frames) || res.frames.length === 0) {
      showErrorT("error.noFrames", { name: sprite.name });
      return;
    }
    hideError();
    applyDecoded(res, sprite);
  } catch (e) {
    showErrorT("error.decodeFailed", { error: e.message || e });
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
  // Direction count is no longer a user-editable select; #dirNo shows it as x/y.
  $("dir").max = String(Math.max(0, dirs - 1));
  $("dir").value = "0";
  $("frame").max = String(framesPerDir - 1);
  $("frame").value = "0";
  currentDir = 0;
  currentFin = 0;
}

// Draw frame (dir, fin) where absolute index = dir * framesPerDir + fin.
function showFrame(dir, fin) {
  if (isCombatMode() || isViewOwnedMode()) return;   // those modes own the view
  if (!currentFrames) return;
  const idx = dir * framesPerDir + fin;
  const f = currentFrames[idx];
  if (!f) return;
  currentDir = dir;
  currentFin = fin;

  drawComposite($("canvas"), f.rgba_base64, f.width, f.height, zoom);
  $("dirNo").textContent = `${dir + 1}/${currentDirections}`;
  $("frameNo").textContent = t("stage.frameInfo", {
    dir, maxDir: currentDirections - 1, fin, maxFin: framesPerDir - 1,
  });
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
  setMetaRender(() =>
    t("stage.meta", {
      name: lastSprite.name, format: lastRes.format, w: f.width, h: f.height,
      dir, fin, total: currentFrames.length,
    }) + (lastRes.trn_applied
      ? t("stage.trnApplied", { name: lastRes.trn_name || "on" }) : ""));
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
  $("playBtn").textContent = t("stage.pause");
}
function stopPlay() {
  isPlaying = false;
  if (playTimer) clearInterval(playTimer);
  playTimer = null;
  $("playBtn").textContent = t("stage.play");
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

// 显示名：中文模式下优先中文名 name_zh，仅在中文名与英文名不同（即确有翻译）时
// 附英文名；英文模式直接用英文名。怪物远程攻击通常无翻译，回落到英文名。
// description 用作悬浮提示（后端数据，不翻译）。
function combatLabel(e) {
  if (!e) return "";
  let base = e.name || "";
  if (getLang() === "zh-cn" && e.name_zh && e.name_zh !== e.name)
    base = `${e.name_zh} (${e.name})`;
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
    kk.className = "ci-key"; kk.textContent = k + SEP();
    const vv = document.createElement("span");
    vv.className = "ci-val"; vv.textContent = v;
    row.appendChild(kk); row.appendChild(vv); el.appendChild(row);
  };
  addRow(t("combat.nameEn"), combatEntry.name);
  if (combatEntry.name_zh && combatEntry.name_zh !== combatEntry.name)
    addRow(t("combat.nameZh"), combatEntry.name_zh);
  if (combatEntry.level)
    addRow(t("combat.spellLevel"), combatEntry.level);
  if (combatEntry.variant) addRow(t("combat.attackForm"), combatEntry.variant);
  if (combatEntry.condition) addRow(t("combat.trigger"), combatEntry.condition);
  if (combatEntry.trigger) addRow(t("combat.trigger"), combatEntry.trigger);
  if (combatEntry.source_ref) addRow(t("combat.sourceRef"), combatEntry.source_ref);
  if (Array.isArray(combatEntry.objects) && combatEntry.objects.length)
    addRow(t("combat.hostObjects"), combatEntry.objects.join(" / "));
  if (Array.isArray(combatEntry.mana_by_level) && combatEntry.mana_by_level.length) {
    const txt = combatEntry.mana_by_level
      .map((o) => t("combat.manaPerLevel", { level: o.level, mana: o.mana }))
      .join(getLang() === "en" ? ", " : "，");
    addRow(t("combat.manaByLevel"), txt);
  }
  // 各级视觉参数（持续时间 / 范围 / 弹射物数量 / 弹速 / 法术特有字段）
  if (Array.isArray(combatEntry.visual_by_level) && combatEntry.visual_by_level.length) {
    const wrap = document.createElement("div");
    wrap.className = "ci-visual";
    const title = document.createElement("div");
    title.className = "ci-key"; title.textContent = t("combat.visualByLevel") + SEP();
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
    const th0 = document.createElement("th"); th0.textContent = t("combat.level"); thead.appendChild(th0);
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
      n.className = "ci-note"; n.textContent = t("combat.note", { text: note });
      wrap.appendChild(n);
    }
    el.appendChild(wrap);
  } else {
    addRow(t("combat.visualByLevel"), t("combat.noVisualScaling"));
  }
  if (combatEntry.description) {
    const d = document.createElement("div");
    d.className = "ci-desc";
    const kk = document.createElement("span");
    kk.className = "ci-key"; kk.textContent = t("combat.description") + SEP();
    const vv = document.createElement("span");
    vv.className = "ci-val"; vv.textContent = combatEntry.description;
    d.appendChild(kk); d.appendChild(vv); el.appendChild(d);
  }
}

function populateCombatSpells() {
  const sel = $("combatSpell");
  const prev = sel.value;
  sel.innerHTML = "";
  for (const e of combatList()) {
    const o = document.createElement("option");
    o.value = e.id;
    o.textContent = combatLabel(e) + (e.monster_ai && !e.variant ? `  (${e.monster_ai})` : "");
    o.title = e.description || e.name;
    sel.appendChild(o);
  }
  if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
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
    o.textContent = `${s.anim || s.type} (${t("entity.framesPerDir", { n: s.frames_per_dir || "?" })})`;
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
  setMetaT("status.loadingCombat");
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
    showErrorT("error.combatLoad", { error: e.message || e });
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
  // Combat uses a fixed 8-way scene direction: x/y = current/total.
  $("dirNo").textContent = `${combatDir + 1}/8`;
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
  $("dirNo").textContent = `${combatDir + 1}/8`;
  const label = stageLabel(st);
  $("frameNo").textContent = t("combat.frameInfo", {
    dir: combatDir, stage: label, fi, total: total - 1,
  });
  $("stageNo").textContent = `${si + 1}/${stages.length} ${label}`;
  $("frame").max = String(total - 1);
  $("frame").value = String(Math.min(fi, total - 1));
  renderCombatInfo();
  setMetaT("combat.meta", {
    name: combatLabel(combatEntry), stage: label,
    layers: st.layers.length, dir: combatDir + 1,
  });
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
  $("playBtn").textContent = t("stage.pause");
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
    showErrorT("error.spellListUnavailable");
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
  zoom: 1,
  panX: 0,
  panY: 0,
  w: 0,             // preview natural width
  h: 0,             // preview natural height
  loaded: false,
  loading: false,
  dragging: false,
  dragX: 0,
  dragY: 0,
  // Phase 2 front-end lighting state:
  tiles: null,      // { layout, tileAtlas, specialAtlas, gw, gh, ambient, softFalloff, lights, light }
  lightOn: false,   // mirrors sceneLightToggle
  torchDragging: false,
  // Phase 3 door local toggle state:
  doors: [],        // [{ x, y, open, closed, idx }]
  doorOpen: null,   // Set<idx> of currently-open doors
  doorOverride: null, // { idx -> open micro } passed to renderScene
  doorEls: [],      // door marker DOM elements, parallel to sceneState.doors
  doorEdit: false,  // mirrors sceneDoorEditToggle
};
let sceneCatalog = [];

// Discrete zoom levels for the scene viewer. Includes sub-1x ratios (0.25/0.5)
// so the whole map can be seen at once; 1x..8x for close inspection. The wheel
// handler steps through this list by index, and sceneSetZoom keeps the viewport
// centre anchored to the same map point.
const SCENE_ZOOMS = [0.25, 0.5, 1, 2, 3, 4, 6, 8];

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
      el.onerror = () => reject(new Error(t("scene.pngDecodeFailed")));
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

// Derive the plain-HTTP origin from the WS bridge URL so scene artifacts can be
// fetched from the mcp-server's file endpoint on the SAME port (ws:// -> http://).
function sceneHttpOrigin() {
  const raw = ($("wsUrl").value || "ws://localhost:18765").trim();
  try {
    const u = new URL(raw);
    return (u.protocol === "wss:" ? "https://" : "http://") + u.host;
  } catch {
    return "http://localhost:18765";
  }
}

// Load a scene PNG from the backend HTTP endpoint (native resolution) and blit it
// onto `canvas`. The backend serves /scenes/<id>.png etc. on the WS port; this
// keeps the full native image out of the JSON-RPC payload. Empty URL clears.
async function sceneDrawUrl(canvas, url) {
  if (!url) {
    const ctx = canvas.getContext("2d");
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    canvas.width = 0;
    canvas.height = 0;
    return { w: 0, h: 0 };
  }
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(t("scene.httpError", { status: resp.status }));
  const blob = await resp.blob();
  let bitmap, revoke = null, close = false;
  if (typeof createImageBitmap === "function") {
    bitmap = await createImageBitmap(blob);
    close = true;
  } else {
    const u = URL.createObjectURL(blob);
    revoke = u;
    bitmap = await new Promise((resolve, reject) => {
      const el = new Image();
      el.onload = () => resolve(el);
      el.onerror = () => reject(new Error(t("scene.pngDecodeFailed")));
      el.src = u;
    });
  }
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

// ---- Phase 2: front-end lighting (real-time, zero server round-trip) --------
// The scene base is rendered locally from the `scene.tiles` index atlas + layout
// (see renderScene), so changing ambient / moving the torch only recomputes a
// per-cell dLight map (buildLightMap) and re-blits — no scene.build call.

// Fetch a PNG over HTTP and return a plain pixel buffer {width,height,data}
// (RGBA Uint8ClampedArray), the same shape renderScene / the Node regression
// test consume.
async function fetchPngBuffer(url) {
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(t("scene.httpError", { status: resp.status }));
  const blob = await resp.blob();
  const bitmap = await createImageBitmap(blob);
  try {
    const c = document.createElement("canvas");
    c.width = bitmap.width; c.height = bitmap.height;
    const ctx = c.getContext("2d");
    ctx.drawImage(bitmap, 0, 0);
    const data = ctx.getImageData(0, 0, c.width, c.height).data;
    return { width: c.width, height: c.height, data };
  } finally {
    if (typeof bitmap.close === "function") bitmap.close();
  }
}

// Call scene.tiles (same args as scene.build) and pull the two index atlases.
async function loadSceneTiles(args) {
  const res = await client.callTool("scene.tiles", args, pushSceneProgress);
  if (!res || !res.layout) throw new Error(t("error.sceneTilesNoLayout"));
  const layout = res.layout;
  const origin = sceneHttpOrigin();
  const tileAtlas = await fetchPngBuffer(origin + res.tiles_url);
  let specialAtlas = null;
  if (res.special_url) {
    try { specialAtlas = await fetchPngBuffer(origin + res.special_url); }
    catch (e) { console.warn("special atlas load failed:", e); specialAtlas = null; }
  }
  return { layout, tileAtlas, specialAtlas };
}

// Render the scene base from the cached tiles into #sceneBase (native res).
function renderSceneBase() {
  const t = sceneState.tiles;
  if (!t) return;
  const out = renderScene(t.layout, t.tileAtlas, t.specialAtlas, {
    light: t.light,
    microOverride: (sceneState.doorOverride && Object.keys(sceneState.doorOverride).length)
      ? sceneState.doorOverride : null,
  });
  const canvas = $("sceneBase");
  canvas.width = out.width;
  canvas.height = out.height;
  const ctx = canvas.getContext("2d");
  ctx.imageSmoothingEnabled = false;
  ctx.putImageData(new ImageData(out.data, out.width, out.height), 0, 0);
  sceneState.w = out.width;
  sceneState.h = out.height;
}

let sceneReRenderScheduled = false;
function scheduleSceneReRender() {
  if (sceneReRenderScheduled) return;
  sceneReRenderScheduled = true;
  requestAnimationFrame(() => {
    sceneReRenderScheduled = false;
    renderSceneBase();
    applySceneTransform();
  });
}

// Recompute the per-cell dLight map from ambient + lights and re-render.
function recomputeSceneLight() {
  const t = sceneState.tiles;
  if (!t || !sceneState.lightOn) return;
  t.light = buildLightMap(t.gw, t.gh, t.ambient, t.lights, t.softFalloff);
  scheduleSceneReRender();
  positionTorchMarker();
}

// Position the torch marker at the (first) light's projected native coords
// within #sceneLayers (which is sized w*zoom, so ×zoom maps native -> layer px).
function positionTorchMarker() {
  const t = sceneState.tiles;
  const marker = $("sceneTorch");
  if (!t || !sceneState.lightOn || !t.lights || !t.lights.length) {
    marker.hidden = true;
    return;
  }
  const L = t.lights[0];
  const proj = t.layout.projection;
  const Hm = (t.layout.micro_len / 2) * 32;
  const nx = (L.x - L.y) * 32 - proj.minX + 32;       // cell centre x
  const ny = (L.x + L.y) * 16 - proj.minY - Hm + 17;  // cell centre y (diamond centre)
  marker.hidden = false;
  marker.style.left = (nx * sceneState.zoom) + "px";
  marker.style.top = (ny * sceneState.zoom) + "px";
}

function showSceneLightControls(visible) {
  $("sceneLightControls").hidden = !visible;
  if (visible && sceneState.tiles) {
    $("sceneAmbient").value = String(sceneState.tiles.ambient | 0);
    $("sceneAmbientVal").textContent = String(sceneState.tiles.ambient | 0);
  }
}

// ---- Phase 3: door local toggle -------------------------------------------
// Doors are children of #sceneLayers (projected native coords × zoom), so they
// track pan/zoom automatically; only left/top are recomputed here.
function doorNativeXY(d) {
  const t = sceneState.tiles;
  const proj = t.layout.projection;
  const Hm = (t.layout.micro_len / 2) * 32;
  const nx = (d.x - d.y) * 32 - proj.minX + 32;       // cell centre x
  const ny = (d.x + d.y) * 16 - proj.minY - Hm + 17;  // cell centre y (diamond centre)
  return { nx, ny };
}

function positionDoorMarkers() {
  const layers = $("sceneLayers");
  const markerLayer = layers; // markers live inside #sceneLayers (see index.html)
  for (let k = 0; k < sceneState.doorEls.length; ++k) {
    const el = sceneState.doorEls[k];
    const d = sceneState.doors[k];
    if (!d) { el.hidden = true; continue; }
    const { nx, ny } = doorNativeXY(d);
    const open = sceneState.doorOpen.has(d.idx);
    el.hidden = !sceneState.doorEdit;
    el.classList.toggle("open", open);
    el.style.left = (nx * sceneState.zoom) + "px";
    el.style.top = (ny * sceneState.zoom) + "px";
  }
}

// (Re)build the door marker elements once per scene load.
function buildDoorMarkers() {
  const layers = $("sceneLayers");
  // Remove any existing door markers.
  sceneState.doorEls.forEach((el) => el.remove());
  sceneState.doorEls = [];
  for (const d of sceneState.doors) {
    const el = document.createElement("div");
    el.className = "scene-door-marker";
    el.hidden = true;
    el.title = "door";
    el.addEventListener("click", (e) => {
      e.stopPropagation();
      toggleDoor(d.idx);
    });
    layers.appendChild(el);
    sceneState.doorEls.push(el);
  }
}

function showDoorControls(visible) {
  $("sceneDoorControls").hidden = !visible;
  if (visible) buildDoorMarkers();
  positionDoorMarkers();
}

function toggleDoor(idx) {
  const t = sceneState.tiles;
  if (!t) return;
  const d = sceneState.doors.find((o) => o.idx === idx);
  if (!d) return;
  if (sceneState.doorOpen.has(idx)) {
    sceneState.doorOpen.delete(idx);
    delete sceneState.doorOverride[idx];
  } else {
    sceneState.doorOpen.add(idx);
    sceneState.doorOverride[idx] = d.open;
  }
  // Re-render the base (micro swap) + a brief flash as a transition cue.
  renderSceneBase();
  applySceneTransform();
  const el = sceneState.doorEls[sceneState.doors.findIndex((o) => o.idx === idx)];
  if (el) {
    el.classList.toggle("open", sceneState.doorOpen.has(idx));
    el.classList.remove("pulse");
    void el.offsetWidth; // restart the CSS animation
    el.classList.add("pulse");
  }
  const base = $("sceneBase");
  base.classList.remove("door-flip");
  void base.offsetWidth;
  base.classList.add("door-flip");
}

// Map a pointer event to a map-native coordinate, then invert the isometric
// projection to a (possibly fractional) micro cell, and move the (first) light.
function updateTorchFromPointer(e) {
  const t = sceneState.tiles;
  if (!t || !sceneState.lightOn) return;
  const vp = $("sceneViewport");
  const rect = vp.getBoundingClientRect();
  const nx = (e.clientX - rect.left - sceneState.panX) / sceneState.zoom;
  const ny = (e.clientY - rect.top - sceneState.panY) / sceneState.zoom;
  const proj = t.layout.projection;
  const Hm = (t.layout.micro_len / 2) * 32;
  const u = (nx + proj.minX) / 32;        // i - j
  const v = (ny + proj.minY + Hm - 1) / 16; // i + j (floor diamond top)
  const ci = (u + v) / 2, cj = (v - u) / 2;
  const x = Math.max(0, Math.min(t.gw - 1, Math.round(ci)));
  const y = Math.max(0, Math.min(t.gh - 1, Math.round(cj)));
  const fx = ci - Math.floor(ci), fy = cj - Math.floor(cj);
  const offX = Math.max(0, Math.min(7, Math.round(fx * 8)));
  const offY = Math.max(0, Math.min(7, Math.round(fy * 8)));
  const lights = t.lights;
  if (!lights.length) lights.push({ x, y, radius: 15, offset_x: offX, offset_y: offY });
  else { lights[0].x = x; lights[0].y = y; lights[0].offset_x = offX; lights[0].offset_y = offY; }
  recomputeSceneLight();
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
  // Phase 2: keep the torch marker glued to its map point while panning/zooming.
  if (sceneState.tiles && sceneState.lightOn) positionTorchMarker();
  // Phase 3: keep the door markers glued to their map points while panning/zooming.
  if (sceneState.doorEdit && sceneState.doorEls.length) positionDoorMarkers();
}

function sceneFit() {
  const vp = $("sceneViewport");
  if (!sceneState.loaded) return;
  // Fit at zoom 1 and centre the MAP's centre in the viewport. Allowing a
  // negative pan (when the scene is larger than the viewport) scrolls so the
  // middle of the map — not its top-left corner — sits in the middle of the view.
  const z = 1;
  sceneState.zoom = z;
  $("sceneZoom").value = String(z);
  sceneState.panX = (vp.clientWidth - sceneState.w * z) / 2;
  sceneState.panY = (vp.clientHeight - sceneState.h * z) / 2;
  applySceneTransform();
}

// Change zoom while keeping the MAP POINT currently under the viewport centre
// fixed there. This avoids the view snapping back to the top-left of the map
// when the user adjusts the zoom (the prior behaviour left pan unchanged, so the
// centre of the screen jumped to a different map coordinate on every zoom).
function sceneSetZoom(newZoom) {
  if (!sceneState.loaded) return;
  newZoom = Math.max(0.25, Math.min(8, newZoom));
  const oldZoom = sceneState.zoom;
  if (oldZoom === newZoom) return;
  const vp = $("sceneViewport");
  const cx = vp.clientWidth / 2;
  const cy = vp.clientHeight / 2;
  // Map coordinate (in native preview px) currently sitting under the centre.
  const mx = (cx - sceneState.panX) / oldZoom;
  const my = (cy - sceneState.panY) / oldZoom;
  // Re-pin that same map point to the centre at the new zoom.
  sceneState.zoom = newZoom;
  sceneState.panX = cx - mx * newZoom;
  sceneState.panY = cy - my * newZoom;
  $("sceneZoom").value = String(newZoom);
  $("sceneZoomNo").textContent = `${newZoom}×`;
  applySceneTransform();
}

// Nearest index in SCENE_ZOOMS to the current zoom (used by the wheel handler).
function sceneZoomIndex() {
  let best = 0, bestD = Infinity;
  for (let i = 0; i < SCENE_ZOOMS.length; i++) {
    const d = Math.abs(SCENE_ZOOMS[i] - sceneState.zoom);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

// ---- Scene build loading mask ------------------------------------------------
// Blocks the whole UI and mirrors backend progress (pushSceneProgress) while a
// scene is rendering. Shown the moment loadScene starts and hidden on finish.
function showSceneLoading(stage) {
  $("sceneLoadingStage").textContent = stage || t("status.scenePreparing");
  $("sceneLoadingFill").style.width = "0%";
  $("sceneLoadingPct").textContent = "0%";
  $("sceneLoadingMask").hidden = false;
}
function pushSceneProgress(pct, stage) {
  const clamped = Math.max(0, Math.min(100, pct | 0));
  $("sceneLoadingFill").style.width = clamped + "%";
  $("sceneLoadingPct").textContent = clamped + "%";
  if (stage) $("sceneLoadingStage").textContent = stage;
}
function hideSceneLoading() {
  $("sceneLoadingMask").hidden = true;
}

// Rebuild the scene dropdown from the cached scene.list payload (also used on a
// language switch, so no extra backend call is needed).
function renderSceneOptions() {
  const sel = $("sceneSel");
  const prev = sel.value;
  sel.innerHTML = "";
  // No default selection: the dropdown starts empty so the viewer does NOT
  // auto-load any scene (the user picks one explicitly). This pairs with the
  // backend cache — once a scene IS built, re-selecting it is instant.
  const empty = document.createElement("option");
  empty.value = "";
  empty.textContent = t("scene.none");
  sel.appendChild(empty);
  for (const s of sceneCatalog) {
    const o = document.createElement("option");
    o.value = s.id;
    o.textContent = t("scene.option", {
      title: dataTitle("scene", s.id, s.title), w: s.grid_width, h: s.grid_height,
    });
    sel.appendChild(o);
  }
  if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
}
async function loadSceneCatalog() {
  try {
    const res = await client.callTool("scene.list", {});
    sceneCatalog = (res && res.scenes) || [];
    renderSceneOptions();
    $("sceneSel").value = "";
  } catch (e) {
    showErrorT("error.sceneList", { error: e.message || e });
  }
}

async function loadScene() {
  if (sceneState.loading) return;
  const id = $("sceneSel").value;
  if (!id) return;   // nothing selected — don't auto-load a default scene
  hideError();
  sceneState.loading = true;
  showSceneLoading(t("scene.loading", { id }));
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
      showErrorT("error.sceneNoArchive");
      return;
    }
    args.preview_max_width = 1000;
    args.lighting = !!$("sceneLightToggle").checked;
    // Dungeon presets are cropped to the .dun bbox by default; the optional
    // Dirt border restores the faithful full dPiece (and hides the room).
    args.dirt_border = !!$("sceneDirtBorderToggle").checked;
    // The onProgress callback streams backend $/progress notifications into the
    // loading mask in real time.
    // ---- Phase 2: render the base locally from scene.tiles (index atlas +
    // layout); scene.build is only used for the aligned overlay PNG + metadata.
    sceneState.lightOn = !!$("sceneLightToggle").checked;

    // 1) tiles: atlas + layout for the front-end base render. ALWAYS request
    //    lighting:false — the front-end owns lighting end-to-end (ambient slider
    //    + draggable torch), recomputing the dLight map locally via buildLightMap.
    //    Fetching with lighting:true would make the server ship a precomputed
    //    layout.light that renderScene falls back to when the user turns lighting
    //    OFF (opts.light=null), wrongly lighting the scene against their wish.
    let tiles = null;
    try {
      tiles = await loadSceneTiles({ ...args, lighting: false });
    } catch (e) {
      console.warn("scene.tiles failed, will fall back to server PNG:", e);
      tiles = null;
    }

    // 2) scene.build: overlay + metadata (lighting-independent overlay, so keep
    //    it off to avoid an unused lit base render on the server).
    let buildRes = null;
    try {
      buildRes = await client.callTool("scene.build",
        { ...args, lighting: false }, pushSceneProgress);
    } catch (e) {
      console.warn("scene.build failed:", e);
    }

    if (tiles) {
      const L = tiles.layout;
      const lights = Array.isArray(L.lights)
        ? L.lights.map((o) => ({ ...o })) : [];
      const ambient = (L.default_ambient != null) ? L.default_ambient
        : (L.ambient || 0);
      sceneState.tiles = {
        layout: L, tileAtlas: tiles.tileAtlas, specialAtlas: tiles.specialAtlas,
        gw: L.grid_width, gh: L.grid_height, ambient,
        softFalloff: !!L.soft_falloff, lights,
        light: sceneState.lightOn
          ? buildLightMap(L.grid_width, L.grid_height, ambient, lights, !!L.softFalloff)
          : null,
      };
      // Phase 3: collect door definitions (closed <-> open micro swap). Start
      // fully CLOSED (the layout ships the closed state), so the open override
      // set begins empty.
      sceneState.doors = Array.isArray(L.doors)
        ? L.doors.map((d) => ({ x: d.x, y: d.y, open: d.open, closed: d.closed, idx: d.y * L.grid_width + d.x }))
        : [];
      sceneState.doorOpen = new Set();
      sceneState.doorOverride = {};
      renderSceneBase();
      showDoorControls(sceneState.doorEdit);
    } else if (buildRes && buildRes.image_base64) {
      // Fallback: draw the server base PNG directly.
      const dim = await sceneDrawUrl($("sceneBase"), sceneHttpOrigin() + buildRes.image_url);
      sceneState.w = dim.w; sceneState.h = dim.h;
    } else {
      throw new Error(t("error.sceneTilesNoLayout"));
    }

    // Overlay (grid / SOL markers) from scene.build, aligned to the tiles base.
    if (buildRes && buildRes.overlay_url) {
      try {
        await sceneDrawUrl($("sceneOverlay"), sceneHttpOrigin() + buildRes.overlay_url);
      } catch (e) { console.warn("overlay load failed:", e); }
    }
    $("sceneOverlay").hidden = !buildRes || !buildRes.overlay_url || !$("sceneOverlayToggle").checked;

    // Night-palette scenes (e.g. Tristram) render very dark and look blank;
    // the brighten toggle defaults to ON, so apply its state on every load.
    $("sceneLayers").classList.toggle("bright", $("sceneBrightToggle").checked);
    sceneState.loaded = true;
    sceneFit();
    showSceneLightControls(sceneState.lightOn);
    // Mirror the GUI layout viewer: show one consolidated scene summary in the
    // top-right "当前资源" line (#currentName) and keep the bottom status bar
    // free of duplicates. The shared sprite control bar (#stageControls) is
    // hidden in scene mode (see setMode), so show the info here instead.
    setMetaT("scene.summary", {
      title: dataTitle("scene", (buildRes && buildRes.scene_id) || id, (buildRes && buildRes.title) || id),
      w: sceneState.w, h: sceneState.h,
      pw: buildRes ? buildRes.preview_width : null, ph: buildRes ? buildRes.preview_height : null,
      gw: sceneState.tiles ? sceneState.tiles.gw : (buildRes ? buildRes.grid_width : null),
      gh: sceneState.tiles ? sceneState.tiles.gh : (buildRes ? buildRes.grid_height : null),
      scale: buildRes ? buildRes.scale : null,
      tp: buildRes ? buildRes.transparent_pixel_count : null, solid: buildRes ? buildRes.solid_count : null,
      light: sceneState.lightOn ? t("common.on") : t("common.off"),
    });
  } catch (e) {
    showErrorT("error.sceneBuild", { error: e.message || e });
    setMetaT("error.sceneLoadFailed", null, true);
  } finally {
    sceneState.loading = false;
    hideSceneLoading();
  }
}

async function showSceneView() {
  // Do NOT auto-load any scene on entry: the dropdown starts empty (no default
  // selection), so the viewer waits for the user to pick one. We only (re)load
  // when a scene is already selected — e.g. returning to the tab after switching
  // modes, or after a cache clear.
  if (!sceneCatalog.length) await loadSceneCatalog();
  if (sceneState.loaded) { sceneFit(); return; }
  const id = $("sceneSel").value;
  if (id && allArchives && allArchives.length && !sceneState.loading) {
    await loadScene();
  } else {
    // No selection yet: leave a clear prompt instead of an empty canvas.
    setMetaT("scene.noSelection");
  }
}

function wireSceneControls() {
  // Auto-load: selecting a scene (or toggling lighting) immediately rebuilds it,
  // no explicit "load" button needed.
  $("sceneSel").addEventListener("change", () => loadScene());
  // Zoom dropdown: discrete levels including sub-1x (0.25/0.5). sceneSetZoom
  // keeps the viewport centre pinned to the same map point.
  $("sceneZoom").addEventListener("change", (e) => {
    sceneSetZoom(parseFloat(e.target.value) || 1);
  });
  $("sceneOverlayToggle").addEventListener("change", (e) => {
    $("sceneOverlay").hidden = !e.target.checked;
  });
  $("sceneBrightToggle").addEventListener("change", (e) => {
    $("sceneLayers").classList.toggle("bright", e.target.checked);
  });
  $("sceneLightToggle").addEventListener("change", () => {
    // Phase 2: toggle lighting locally — the base is rendered client-side, so
    // turning dLight on/off (and dragging the torch / ambient later) needs no
    // server round-trip. Only fall back to a full reload if tiles aren't loaded.
    sceneState.lightOn = $("sceneLightToggle").checked;
    showSceneLightControls(sceneState.lightOn);
    if (sceneState.tiles) {
      sceneState.tiles.light = sceneState.lightOn
        ? buildLightMap(sceneState.tiles.gw, sceneState.tiles.gh,
                        sceneState.tiles.ambient, sceneState.tiles.lights,
                        sceneState.tiles.softFalloff)
        : null;
      scheduleSceneReRender();
      positionTorchMarker();
    } else {
      loadScene();
    }
  });
  // Ambient slider: real-time re-shade, no server call.
  $("sceneAmbient").addEventListener("input", (e) => {
    const v = parseInt(e.target.value, 10) || 0;
    $("sceneAmbientVal").textContent = String(v);
    if (sceneState.tiles) {
      sceneState.tiles.ambient = v;
      recomputeSceneLight();
    }
  });
  // Torch marker drag -> move the (first) point light and re-shade.
  const torch = $("sceneTorch");
  torch.addEventListener("pointerdown", (e) => {
    e.stopPropagation();
    sceneState.torchDragging = true;
    try { torch.setPointerCapture(e.pointerId); } catch (_) {}
  });
  torch.addEventListener("pointermove", (e) => {
    if (!sceneState.torchDragging) return;
    updateTorchFromPointer(e);
  });
  torch.addEventListener("pointerup", (e) => {
    sceneState.torchDragging = false;
    try { torch.releasePointerCapture(e.pointerId); } catch (_) {}
  });
  $("sceneDirtBorderToggle").addEventListener("change", () => {
    // Rebuild: the border changes grid size / DUN offset server-side.
    loadScene();
  });
  // Phase 3: door edit toggle — show/hide clickable door markers. Toggling only
  // reveals the markers; the actual open/closed swap is a local canvas edit.
  $("sceneDoorEditToggle").addEventListener("change", () => {
    sceneState.doorEdit = $("sceneDoorEditToggle").checked;
    showDoorControls(sceneState.doorEdit);
  });
  $("sceneFitBtn").addEventListener("click", () => sceneFit());
  // Clear the on-disk cache (native/overlay/preview PNGs + metadata + cache
  // markers) and rebuild the current scene fresh if one is selected.
  $("sceneClearCacheBtn").addEventListener("click", async () => {
    if (sceneState.loading) return;
    try {
      const res = await client.callTool("scene.clearCache", { out: "out/scenes" });
      const n = (res && res.cleared) || 0;
      setMetaT("status.cacheCleared", { count: n });
      const id = $("sceneSel").value;
      if (id && allArchives && allArchives.length) {
        await loadScene();   // cache is gone, so this does a fresh server-side build
      }
    } catch (e) {
      showErrorT("error.sceneCacheClear", { error: e.message || e });
    }
  });

  const vp = $("sceneViewport");
  vp.addEventListener("wheel", (e) => {
    if (!isSceneMode()) return;
    e.preventDefault();
    const dir = e.deltaY < 0 ? 1 : -1;
    const idx = Math.max(0, Math.min(SCENE_ZOOMS.length - 1, sceneZoomIndex() + dir));
    sceneSetZoom(SCENE_ZOOMS[idx]);
  }, { passive: false });
  vp.addEventListener("mousedown", (e) => {
    if (!isSceneMode()) return;
    // The torch marker has its own pointer drag; don't start a map pan on it.
    if (e.target && e.target.classList && e.target.classList.contains("scene-torch")) return;
    // Door markers are click-to-toggle; don't start a pan on them either.
    if (e.target && e.target.classList && e.target.classList.contains("scene-door-marker")) return;
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

const guiState = {
  catalog: [],        // ui.list screens
  elements: [],       // ui.build elements for the current screen
  selectedIndex: -1,  // dropdown-selected element (re-highlighted on zoom)
  screenId: "",       // screen whose elements are currently shown
  zoom: 1,            // default display magnification for the UI layout viewer
  loaded: false,
  loading: false,
};

// Monotonic request counter. EVERY loadGuiScreen() call advances it, including
// the ones dropped by the in-flight guard, so an active build can detect that a
// newer request arrived even when the screen id did not change (e.g. toggling
// number tags during a build). Comparing the DOM value against a captured id
// is not enough: it misses same-screen reload requests entirely.
let guiLoadSeq = 0;

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

function renderGuiOptions() {
  const sel = $("guiSel");
  const prev = sel.value;
  sel.innerHTML = "";
  for (const s of guiState.catalog) {
    const o = document.createElement("option");
    o.value = s.id;
    o.textContent = t("gui.screenOption", {
      title: dataTitle("screen", s.id, s.title), count: s.element_count,
    });
    sel.appendChild(o);
  }
  if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
}
async function loadGuiCatalog() {
  try {
    const res = await client.callTool("ui.list", {});
    guiState.catalog = (res && res.screens) || [];
    renderGuiOptions();
    if (guiState.catalog.length) $("guiSel").value = "mainmenu";
  } catch (e) {
    showErrorT("error.uiList", { error: e.message || e });
  }
}

async function loadGuiScreen() {
  const seq = ++guiLoadSeq;
  const id = $("guiSel").value || "mainmenu";
  // Preserve the selected element only when reloading the SAME screen (number
  // tags toggle / manual reload); a genuine screen switch starts unselected.
  const preserveIndex = (id === guiState.screenId) ? guiState.selectedIndex : -1;
  const stale = () => seq !== guiLoadSeq;
  if (guiState.loading) {
    // Don't run a second concurrent build. This call already advanced
    // guiLoadSeq, so the active build sees `stale()` and re-issues in `finally`
    // with the freshest parameters.
    return;
  }
  hideError();
  setMetaT("gui.loading", { id });
  guiState.loading = true;
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
      showErrorT("error.uiNoArchive");
      setMetaT("error.uiLoadFailed", null, true);
      return;
    }
    const res = await client.callTool("ui.build", args);
    if (!res || !res.image_base64) throw new Error(t("error.uiBuildNoImage"));
    if (stale()) return; // newer request; re-issued in `finally`
    await sceneDrawPng($("guiBase"), res.image_base64);
    await sceneDrawPng($("guiOverlay"), res.overlay_base64 || "");
    if (stale()) return;
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
    // One consolidated summary line in the top-right "当前资源" area; keep the
    // bottom status bar free of duplicates.
    const pals = (res.palettes || []).map((p) => p.name.split(/[\\/]/).pop()).join(", ") || t("gui.embedded");
    setMetaT("gui.summary", {
      title: dataTitle("screen", res.id, res.title),
      count: res.element_count, boxes: res.overlay_box_count,
      ok: res.art_ok_count, missing: res.art_missing_count,
      errorCount: res.art_error_count, pals, tp: res.transparent_pixel_count,
    });
  } catch (e) {
    clearGuiElementState();
    showErrorT("error.uiBuild", { error: e.message || e });
    setMetaT("error.uiLoadFailed", null, true);
  } finally {
    guiState.loading = false;
    // Any newer request (same screen OR a different one) that arrived while
    // this build was in flight is honoured here: clear the view, then rebuild
    // with the freshest parameters. The re-issued call gets its own sequence
    // number, so this terminates once no further request is pending.
    if (stale()) {
      resetGuiElementView();
      loadGuiScreen();
    }
  }
}

async function showGuiView() {
  if (!guiState.catalog.length) await loadGuiCatalog();
  if (guiState.loaded) { applyGuiTransform(); return; }
  if (allArchives && allArchives.length && !guiState.loading) {
    await loadGuiScreen();
  }
}

// Clear the three stacked canvases so a failed build never leaves the previous
// screen's picture (or a partially drawn frame) behind.
function clearGuiCanvases() {
  for (const id of ["guiBase", "guiText", "guiOverlay"]) {
    const c = $(id);
    c.getContext("2d").clearRect(0, 0, c.width, c.height);
  }
}

// View-only reset: dropdown shows just the placeholder, detail pane shows the
// prompt and the highlight box is hidden. Selection bookkeeping is left intact
// so a same-screen re-issue can restore the previously selected element.
function resetGuiElementView() {
  const sel = $("guiElementSel");
  sel.innerHTML = "";
  const ph = document.createElement("option");
  ph.value = "";
  ph.textContent = t("gui.selectElement");
  sel.appendChild(ph);
  sel.value = "";
  showGuiElementDetail(null);
}

// Reset element state (view + bookkeeping) and clear the canvases. Called on
// init and on any failed or aborted build, so stale per-screen data from a
// previous screen never lingers when a load does not succeed.
function clearGuiElementState() {
  guiState.elements = [];
  guiState.selectedIndex = -1;
  guiState.screenId = "";
  guiState.loaded = false;
  clearGuiCanvases();
  populateGuiElementSelect();
}

// Populate the element dropdown from the current screen's ui.build elements.
// ALL elements are listed (including text-only ones with no backend art), so
// users can inspect any element without having to hover its (possibly tiny or
// overlapping) rectangle on the canvas. `preserveIndex` restores a selection
// when reloading the SAME screen (number-tag toggle / manual reload).
function populateGuiElementSelect(preserveIndex = -1) {
  const sel = $("guiElementSel");
  resetGuiElementView();
  // Keep entries as-is (no filtering) so the 1..N numbering still matches the
  // backend overlay number tags; just render defensively if a field is missing.
  guiState.elements.forEach((el, i) => {
    const o = document.createElement("option");
    o.value = String(i);
    const label = (el && (el.label || el.id)) || t("gui.elementN", { n: i + 1 });
    const type = (el && el.type) || "?";
    o.textContent = t("gui.elementOption", { n: i + 1, label, type });
    sel.appendChild(o);
  });
  guiState.selectedIndex = -1;
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
  if (!el) {
    box.hidden = true;
    $("guiInfo").textContent = t("gui.infoPrompt");
    return;
  }
  const r = Array.isArray(el.rect) ? el.rect : [0, 0, 0, 0];
  const z = guiState.zoom;
  box.style.left = `${r[0] * z}px`;
  box.style.top = `${r[1] * z}px`;
  box.style.width = `${r[2] * z}px`;
  box.style.height = `${r[3] * z}px`;
  box.hidden = false;
  // All backend-provided values are escaped; only the static <br> in the art
  // row and the wrapper tags are trusted markup.
  const row = (k, v) =>
    `<div class="gi-row"><span class="gi-key">${esc(k)}</span><span class="gi-val">${v}</span></div>`;
  const a = el.art;
  let html = `<div class="gi-title">${esc(el.label || el.id || "—")}</div>`;
  html += row(t("gui.infoId"), esc(el.id || "-")) +
          row(t("gui.infoType"), esc(el.type || "-")) +
          row(t("gui.infoRect"), esc(`[${r.join(", ")}]`));
  if (el.panel) html += row(t("gui.infoPanel"), esc(`${el.panel} ${JSON.stringify(el.panel_rect)}`));
  html += row(t("gui.infoZ"), esc(el.z ?? "-"));
  if (a) {
    const art = `${esc(a.name || "-")}<br>${esc(a.format || "-")} · w=${esc(a.width ?? "-")} · ` +
                `frame ${esc(a.frame ?? "-")}/${esc(a.frames ?? "-")}`;
    html += row(t("gui.infoArt"), art) +
            row(t("gui.infoPalette"), esc(a.palette || "-")) +
            row(t("gui.infoDecode"), esc(el.decode_status || "-"));
  }
  if (el.grid) html += row(t("gui.infoGrid"),
    esc(`${el.grid.cols}×${el.grid.rows} · cell ${el.grid.cell} · step ${el.grid.step}`));
  if (el.text) html += row(t("gui.infoText"), esc(guiExampleText(el.text))) +
                       row(t("gui.infoFont"), esc(`${el.font_size} / ${el.color || "-"} / ${el.align || "left"}`));
  html += row(t("gui.infoSource"), esc(el.source_ref || "-"));
  const note = (el.text && /\{/.test(el.text)) ? t("gui.notePlaceholder") : t("gui.noteFont");
  html += `<div class="gi-note">${esc(note)}</div>`;
  $("guiInfo").innerHTML = html;
}

function wireGuiControls() {
  // Initialise the element dropdown with its placeholder even before any
  // screen has loaded (no archives / not connected yet -> still shows a hint).
  clearGuiElementState();
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
    // number tags are baked into the backend overlay PNG -> rebuild. Register
    // the request whenever a screen is loaded OR a build is in flight; the
    // sequence counter dedups/retries it with the freshest parameters. (The
    // guard only avoids a pointless build before the first connect.)
    if (guiState.loaded || guiState.loading) loadGuiScreen();
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
      setMetaT("conn.connectFailed", { error: e.message || e }, true);
    }
  });

  // Browse-mode toggle: Catalog vs merged Entity (character / monster / NPC)
  // vs Combat vs Scene vs GUI panes.
  document.querySelectorAll('input[name="mode"]').forEach((r) => {
    r.addEventListener("change", (e) => {
      if (!e.target.checked) return;
      const entity = e.target.value === "entity";
      const combat = e.target.value === "combat";
      const scene = e.target.value === "scene";
      const gui = e.target.value === "gui";
      const viewOwned = scene || gui;
      stopPlay();
      $("catalogPane").hidden = entity || combat || viewOwned;
      $("entityPane").hidden = !entity;
      $("combatPane").hidden = !combat;
      $("combatDetailPane").hidden = !combat;
      $("scenePane").hidden = !scene;
      $("guiPane").hidden = !gui;
      // Scene / GUI mode swaps the shared canvas for its own PNG viewport.
      $("canvasWrap").hidden = viewOwned;
      $("sceneView").hidden = !scene;
      $("guiView").hidden = !gui;
      // The shared sprite/character control bar (palette / TRN / direction /
      // frame / zoom / play / fps) does not apply to the GUI layout viewer or
      // the scene viewer, both of which have their own controls (#guiPane /
      // #scenePane) and show their summary in #currentName — that line now also
      // carries the frame meta text. Hide it so the stage isn't cluttered.
      $("stageControls").hidden = scene || gui;
      // Leaving the scene view should also dismiss any loading mask left behind.
      if (!scene) hideSceneLoading();
      // Combat uses a fixed 8-way scene direction; other modes use the sprite's
      // own direction count when the next sprite is decoded.
      if (combat) syncCombatDirControl();
      else if (!viewOwned) {
        $("dir").max = String(Math.max(0, currentDirections - 1));
        $("dirNo").textContent = `${currentDir + 1}/${currentDirections}`;
      }
      // Entity / combat all want level palettes (correct colours).
      applyPaletteFilter(entity || combat);
      if (entity) syncEntityType();
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
  // Direction count is derived from sprite.decode's `directions` field; the
  // old manual 1/2/4/8 selector is gone and #dirNo displays it as x/y.
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

  // TRN preset alone (with the "无" option) controls the remap: re-decode so
  // the colour change takes effect, including in combat mode.
  $("trnPreset").addEventListener("change", () => {
    if (isViewOwnedMode()) return;
    if (isCombatMode()) { combatCache.clear(); buildCombatScene(); return; }
    if (lastSprite) onSelectSprite(lastSprite);
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

  // Entity viewer controls. The left #entityType dropdown swaps the parameter
  // panel (玩家角色 / 怪物 / NPC); each panel owns its own controls wired below.
  $("entityType").addEventListener("change", () => syncEntityType());
  $("monsterArchive").addEventListener("change", () => showMonsterSprite());
  $("npcArchive").addEventListener("change", () => showNpcSprite());
  $("monsterSel").addEventListener("change", () => {
    fillMonsterAnims();
    showMonsterSprite();
  });
  $("monsterAnim").addEventListener("change", () => showMonsterSprite());
  $("npcSel").addEventListener("change", () => showNpcSprite());

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
  wireMpqConfig();
}

// Rebuild a <select> while keeping the current value (option VALUES are backend
// data; only the visible labels change with the language).
function reselect(id, fn) {
  const sel = $(id);
  const prev = sel.value;
  fn();
  if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
}

// Re-render every runtime-built string after a language switch. applyI18n()
// already re-translated the static markup before this callback fires.
function refreshAllText() {
  client.onStatus(connected);
  reflectMpqConfig();
  $("playBtn").textContent = isPlaying ? t("stage.pause") : t("stage.play");
  renderMeta();
  renderError();
  if (allPalettes.length) applyPaletteFilter(isEntityMode() || isCombatMode());
  if (allArchives.length) populateMonsterArchive();
  if (charIndex) refreshCharOptions();
  if (monsterMap) {
    reselect("monsterAnim", fillMonsterAnims);
    reselect("combatMonsterAnim", fillCombatMonsterAnims);
  }
  if (npcSprites.length) renderNpcOptions();
  if (spellCatalog) reselect("combatSpell", populateCombatSpells);
  if (sceneCatalog.length) renderSceneOptions();
  if (guiState.catalog.length) renderGuiOptions();
  if (guiState.loaded) populateGuiElementSelect(guiState.selectedIndex);
  else showGuiElementDetail(null);
  // Re-render the active view's frame counters / status line.
  if (isCombatMode() && combatEntry) renderCombatStage(combatStage, combatFrame);
  else if (!isViewOwnedMode() && currentFrames) redrawCurrent();
}
onLangChange(refreshAllText);

wireControls();
stopPlay();             // initial localized Play label
client.onStatus(false); // initial localized disconnected status
// P1: reflect the persisted MPQ directory (dialog badge + input + button colour)
// and, if configured, auto-connect + auto-load the catalog. If unconfigured, keep
// the button red and do NOT auto-read the MPQ.
reflectMpqConfig();
initMpqEntry();
