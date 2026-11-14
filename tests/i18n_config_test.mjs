// tests/i18n_config_test.mjs
//
// Frontend i18n test (run with: node tests/i18n_config_test.mjs).
//
// Two layers:
//   1. Config integrity — the two language tables must expose exactly the same
//      key set, the same {placeholder} names per key, and every key referenced
//      from HTML/JS must exist. This is what makes "edit a JSON file" safe.
//   2. Engine behaviour — load web/src/i18n.js with stubbed browser globals and
//      verify t()/tOpt() interpolation, fallback locale, DOM translation,
//      persistence and the live language-switch callback.

import assert from "node:assert/strict";
import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import os from "node:os";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const I18N_DIR = path.join(ROOT, "web", "i18n");
const WEB_DIR = path.join(ROOT, "web");
let checks = 0;
function ok(cond, msg) { assert.ok(cond, msg); checks++; }
function eq(actual, expected, msg) { assert.equal(actual, expected, msg); checks++; }

async function readJson(file) {
  return JSON.parse(await fs.readFile(path.join(I18N_DIR, file), "utf8"));
}

// Recursively flatten a string table into "a.b.c" -> value.
function flatten(obj, prefix = "", out = new Map()) {
  for (const [k, v] of Object.entries(obj)) {
    const key = prefix ? `${prefix}.${k}` : k;
    if (v && typeof v === "object" && !Array.isArray(v)) flatten(v, key, out);
    else out.set(key, v);
  }
  return out;
}

function placeholders(s) {
  const set = new Set();
  for (const m of String(s).matchAll(/\{([A-Za-z0-9_]+)\}/g)) set.add(m[1]);
  return [...set].sort().join(",");
}

// ---------------------------------------------------------------- layer 1 ---
const manifest = await readJson("locales.json");
ok(Array.isArray(manifest.locales) && manifest.locales.length >= 2,
   "locales.json must list at least two languages");
const codes = manifest.locales.map((l) => l.code);
ok(codes.includes("en") && codes.includes("zh-cn"), "en and zh-cn must be registered");
ok(codes.includes(manifest.default), `default "${manifest.default}" must be a registered locale`);
ok(codes.includes(manifest.fallback), `fallback "${manifest.fallback}" must be a registered locale`);
for (const l of manifest.locales) {
  ok(typeof l.label === "string" && l.label.length > 0, `locale ${l.code} needs a label`);
  ok((await fs.stat(path.join(I18N_DIR, `${l.code}.json`))).isFile(), `missing ${l.code}.json`);
}

const tables = new Map();
for (const code of codes) tables.set(code, flatten(await readJson(`${code}.json`)));

const en = tables.get("en");
const zh = tables.get("zh-cn");
ok(en.size > 100, `en.json should be a full string table (got ${en.size} keys)`);
const sortedKeys = (m) => [...m.keys()].sort();
eq(sortedKeys(zh).join("\n"), sortedKeys(en).join("\n"),
   "en and zh-cn must define exactly the same key set");

for (const [code, table] of tables) {
  for (const [key, value] of table) {
    ok(typeof value === "string" && value.length > 0,
       `${code}.json: empty or non-string value at "${key}"`);
  }
}
for (const key of en.keys()) {
  eq(placeholders(zh.get(key)), placeholders(en.get(key)),
     `placeholder mismatch for "${key}" between en and zh-cn`);
}

// Every key referenced from the markup must exist in both tables.
const htmlKeys = new Set();
for (const file of ["index.html", "level.html"]) {
  const html = await fs.readFile(path.join(WEB_DIR, file), "utf8");
  for (const m of html.matchAll(/data-i18n="([^"]+)"/g)) htmlKeys.add(m[1]);
  for (const m of html.matchAll(/data-i18n-attr="([^"]+)"/g)) {
    for (const part of m[1].split(",")) {
      const i = part.indexOf(":");
      if (i > 0) htmlKeys.add(part.slice(i + 1).trim());
    }
  }
}
ok(htmlKeys.size > 50, `expected many data-i18n keys in HTML (got ${htmlKeys.size})`);
for (const key of htmlKeys) {
  ok(en.has(key), `HTML references missing key "${key}" (en)`);
  ok(zh.has(key), `HTML references missing key "${key}" (zh-cn)`);
}

// Every literal key passed to t()/tOpt()/setMetaT()/showErrorT()/setStatusT()
// in the JS modules must exist. Dynamic template-literal keys are checked via
// their group below instead.
const jsKeys = new Set();
for (const file of ["main.js", "level.js", "client.js", "catalog.js", "i18n.js"]) {
  const src = await fs.readFile(path.join(WEB_DIR, "src", file), "utf8");
  for (const m of src.matchAll(/\b(?:t|tOpt|setMetaT|showErrorT|setStatusT)\(\s*"([A-Za-z0-9_.]+)"/g))
    jsKeys.add(m[1]);
  for (const m of src.matchAll(/labelKey:\s*"([A-Za-z0-9_.]+)"/g)) jsKeys.add(m[1]);
}
ok(jsKeys.size > 80, `expected many t() keys in JS (got ${jsKeys.size})`);
for (const key of jsKeys) {
  ok(en.has(key), `JS references missing key "${key}" (en)`);
  ok(zh.has(key), `JS references missing key "${key}" (zh-cn)`);
}
// Dynamic key groups: enum.<group>.<code>, data.<group>.<id>, combat.stage<Id>.
for (const group of ["type", "armor", "weapon"]) {
  ok([...en.keys()].some((k) => k.startsWith(`enum.${group}.`)), `missing enum.${group} group`);
}
for (const [group, ids] of Object.entries({
  screen: ["mainmenu", "title", "credits", "selhero", "selgame", "selok",
           "selyesno", "settings", "control_panel", "char_panel", "inventory",
           "spellbook", "quest_log", "store_list", "store_grid", "dialog_small",
           "dialog_large", "dialog_large_dvl"],
  scene: ["tristram", "l1_rnd2", "l1_skngdo", "l2_blind", "l3_anvil", "l4_diab1",
          "random_l1", "random_l2", "random_l3", "random_l4"],
})) {
  for (const id of ids) {
    ok(en.has(`data.${group}.${id}`), `missing data.${group}.${id}`);
    ok(zh.has(`data.${group}.${id}`), `missing data.${group}.${id}`);
  }
}
for (const key of ["combat.stageCast", "combat.stageProjectile",
                   "combat.stageImpact", "combat.stageDebuff"]) {
  ok(en.has(key) && zh.has(key), `missing ${key}`);
}

// ---------------------------------------------------------------- layer 2 ---
// Copy the engine to a .mjs so Node loads it as an ES module regardless of any
// package.json in the repo. The stubbed fetch reads the REAL config files.
const engineSrc = await fs.readFile(path.join(WEB_DIR, "src", "i18n.js"), "utf8");
const tmpDir = await fs.mkdtemp(path.join(os.tmpdir(), "dhfc-i18n-"));
const enginePath = path.join(tmpDir, "i18n.mjs");
await fs.writeFile(enginePath, engineSrc, "utf8");

const realFetch = async (url) => {
  const file = path.basename(new URL(String(url)).pathname);
  const text = await fs.readFile(path.join(I18N_DIR, file), "utf8");
  return { ok: true, status: 200, json: async () => JSON.parse(text) };
};
let fetchImpl = realFetch;
globalThis.fetch = (url) => fetchImpl(url);

const store = new Map();
globalThis.localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => store.set(k, String(v)),
  removeItem: (k) => store.delete(k),
};
try {
  Object.defineProperty(globalThis, "navigator", {
    value: { language: "en-US" }, configurable: true,
  });
} catch { /* Node's own navigator.language is already en-US. */ }

function fakeEl(attrs = {}) {
  return {
    attrs, textContent: "", innerHTML: "", value: "", options: [],
    getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; },
    setAttribute(k, v) { this.attrs[k] = String(v); },
    appendChild(o) { this.options.push(o); },
    addEventListener(ev, fn) { (this.handlers ||= {})[ev] = fn; },
  };
}
// applyI18n only needs querySelectorAll; the language <select> needs appendChild.
const titleEl = fakeEl({ "data-i18n": "app.title" });
const inputEl = fakeEl({ "data-i18n-attr": "placeholder:mpq.dirPlaceholder,title:mpq.title" });
const langSel = fakeEl({ "data-lang-select": "" });
const nodes = [titleEl, inputEl, langSel];
globalThis.document = {
  documentElement: { lang: "" },
  createElement() { return fakeEl(); },
  querySelectorAll(sel) {
    const attr = sel.slice(1, -1);
    return nodes.filter((n) => attr in n.attrs);
  },
};

const engine = await import(pathToFileURL(enginePath).href);

const info = await engine.initI18n();
eq(info.ok, true, "initI18n should succeed");
eq(engine.getLang(), "en", "en-US browser language selects en on first visit");
eq(engine.isReady(), true, "engine should be ready");
eq(engine.getLocales().length, 2, "two locales registered");
eq(document.documentElement.lang, "en", "html lang attribute set");
eq(engine.t("mode.catalog"), "Catalog", "basic lookup");
eq(engine.t("conn.connectFailed", { error: "boom" }), "connect failed: boom", "interpolation");
eq(engine.tOpt("data.screen.mainmenu", "fallback"), "Main Menu", "data override lookup");
eq(engine.tOpt("no.such.key", "fallback"), "fallback", "explicit fallback");
eq(engine.tOpt("no.such.key"), "no.such.key", "missing key returns the key");
eq(engine.t("missing.key"), "missing.key", "t() missing key returns the key");
eq(await engine.setLang("xx"), false, "unsupported locale rejected");

engine.applyI18n();
eq(titleEl.textContent, "DiabloHFConverter · Sprite & Scene Viewer", "data-i18n translation");
eq(inputEl.attrs.placeholder, "Type or paste the folder containing your legitimate MPQ files",
   "data-i18n-attr placeholder translation");
eq(inputEl.attrs.title, "MPQ Setup", "data-i18n-attr title translation");

let switched = null;
engine.onLangChange((code) => { switched = code; });
eq(await engine.setLang("zh-cn"), true, "switch to zh-cn");
eq(switched, "zh-cn", "onLangChange fired after switch");
eq(store.get("dhfc.lang"), "zh-cn", "language persisted to localStorage");
eq(document.documentElement.lang, "zh-CN", "html lang updated to zh-CN");
eq(engine.t("mode.catalog"), "目录浏览", "zh-cn lookup");
eq(titleEl.textContent, "DiabloHFConverter · 精灵与场景查看器", "static DOM re-translated");
eq(inputEl.attrs.placeholder, "输入或者粘贴你的正版MPQ所在目录", "attributes re-translated");

// Fallback locale: serve an empty zh-cn table and confirm strings come from en.
fetchImpl = async (url) => {
  const file = path.basename(new URL(String(url)).pathname);
  if (file === "zh-cn.json") return { ok: true, status: 200, json: async () => ({}) };
  return realFetch(url);
};
await engine.setLang("en");
eq(await engine.setLang("zh-cn"), true, "switch with sparse zh-cn table");
eq(engine.t("mode.catalog"), "Catalog", "missing keys fall back to en");
eq(await engine.setLang("en"), true, "switch back to en");
eq(engine.t("mode.catalog"), "Catalog", "en restored");
eq(await engine.setLang("en"), false, "same locale is a no-op");

await fs.rm(tmpDir, { recursive: true, force: true });
console.log(`i18n_config_test: PASS (${checks} assertions)`);
