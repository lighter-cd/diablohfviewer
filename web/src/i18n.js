// web/src/i18n.js
//
// Frontend i18n engine. All user-visible strings live in plain JSON config files
// under web/i18n/ (locales.json is the manifest, <code>.json is a string table).
// Adding a language needs no code change: drop a new <code>.json next to the
// others and add one { code, label } entry to locales.json.
//
// Usage:
//   import { t, tOpt, initI18n, applyI18n, wireLangSelectors, onLangChange } ...
//   await initI18n();          // load manifest + current locale (top-level await)
//   applyI18n();               // translate every [data-i18n] / [data-i18n-attr]
//   wireLangSelectors();       // populate every [data-lang-select] <select>
//   t("mode.catalog")          // dynamic string, with {var} interpolation
//   tOpt("data.screen.mainmenu", backendTitle)  // optional override of backend data
//
// Language is persisted in localStorage ("dhfc.lang"). On first visit it is
// detected from navigator.language, falling back to locales.json's "default".
// Missing keys fall back to the "fallback" locale, then to the key itself.

const LS_KEY = "dhfc.lang";
const I18N_DIR = new URL("../i18n/", import.meta.url);

// Cache-busting token. ./sync-web.sh rewrites every ?v=<token> in build-mcp/web
// on each frontend sync, so the JSON tables are never served stale.
const CACHE = "?v=20261113d";

let manifest = { default: "en", fallback: "en", locales: [] };
let strings = {};
let fallbackStrings = {};
let lang = "en";
let ready = false;
const listeners = new Set();

export function getLang() { return lang; }
export function getLocales() { return manifest.locales.slice(); }
export function isReady() { return ready; }

// Register a callback fired AFTER the DOM has been re-translated on a language
// change. Use it to re-render strings built at runtime (status lines, dropdown
// options, etc.). Returns an unregister function.
export function onLangChange(fn) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

// Resolve a dotted key ("gui.infoId") against a nested string table.
function resolve(obj, key) {
  let cur = obj;
  for (const part of key.split(".")) {
    if (cur == null || typeof cur !== "object") return undefined;
    cur = cur[part];
  }
  return cur;
}

// Look up a key without interpolation. `fallback` is returned when the key is
// absent from BOTH the current and fallback tables (defaults to the key itself).
export function tOpt(key, fallback) {
  let v = resolve(strings, key);
  if (v == null) v = resolve(fallbackStrings, key);
  if (typeof v !== "string") return fallback === undefined ? key : fallback;
  return v;
}

// Translate a key, replacing every {name} token with vars[name].
export function t(key, vars) {
  let s = tOpt(key, key);
  if (vars) {
    for (const [k, v] of Object.entries(vars)) {
      s = s.split("{" + k + "}").join(String(v));
    }
  }
  return s;
}

async function loadJson(file) {
  const url = new URL(file + CACHE, I18N_DIR);
  const resp = await fetch(url);
  if (!resp.ok) throw new Error("HTTP " + resp.status + " " + url.href);
  return resp.json();
}

function isSupported(code) {
  return manifest.locales.some((l) => l.code === code);
}

function detectLang() {
  let saved = "";
  try { saved = localStorage.getItem(LS_KEY) || ""; } catch {}
  if (saved && isSupported(saved)) return saved;
  const nav = (navigator.language || "").toLowerCase();
  for (const l of manifest.locales) {
    if (nav === l.code || nav.startsWith(l.code + "-")) return l.code;
  }
  if (nav.startsWith("zh")) return "zh-cn";
  if (nav.startsWith("en")) return "en";
  return isSupported(manifest.default)
    ? manifest.default
    : (manifest.locales[0] ? manifest.locales[0].code : "en");
}

async function loadLocale(code) {
  const fb = manifest.fallback && manifest.fallback !== code ? manifest.fallback : "";
  const [primary, fallback] = await Promise.all([
    loadJson(code + ".json"),
    fb ? loadJson(fb + ".json").catch(() => ({})) : Promise.resolve({}),
  ]);
  strings = primary || {};
  fallbackStrings = fb ? (fallback || {}) : strings;
}

function htmlLang(code) {
  return code === "zh-cn" ? "zh-CN" : code;
}

// Load the manifest + current locale. Never throws: if the JSON files cannot be
// read the engine degrades to the static Chinese text already in the HTML.
export async function initI18n() {
  try {
    manifest = await loadJson("locales.json");
  } catch (e) {
    console.error("i18n: failed to load locales.json", e);
  }
  if (!manifest || !Array.isArray(manifest.locales) || !manifest.locales.length) {
    manifest = {
      default: "zh-cn", fallback: "en",
      locales: [{ code: "en", label: "English" }, { code: "zh-cn", label: "简体中文" }],
    };
  }
  lang = detectLang();
  try {
    await loadLocale(lang);
    ready = true;
  } catch (e) {
    console.error("i18n: failed to load locale " + lang, e);
    strings = {};
    fallbackStrings = {};
    ready = false;
  }
  try { document.documentElement.lang = htmlLang(lang); } catch {}
  return { ok: ready, lang };
}

// Translate all marked DOM nodes. `data-i18n` sets textContent;
// `data-i18n-attr="attr:key,attr:key"` sets arbitrary attributes (placeholder,
// title, aria-label, ...). Both are safe (no innerHTML from config files).
export function applyI18n(root = document) {
  if (!ready) return;
  root.querySelectorAll("[data-i18n]").forEach((el) => {
    el.textContent = t(el.getAttribute("data-i18n"));
  });
  root.querySelectorAll("[data-i18n-attr]").forEach((el) => {
    for (const part of el.getAttribute("data-i18n-attr").split(",")) {
      const i = part.indexOf(":");
      if (i < 0) continue;
      el.setAttribute(part.slice(0, i).trim(), t(part.slice(i + 1).trim()));
    }
  });
}

function syncLangSelectors() {
  try {
    document.querySelectorAll("[data-lang-select]").forEach((sel) => { sel.value = lang; });
  } catch {}
}

// Build the language dropdown(s) from the manifest and wire live switching.
export function wireLangSelectors() {
  document.querySelectorAll("[data-lang-select]").forEach((sel) => {
    sel.innerHTML = "";
    for (const l of manifest.locales) {
      const o = document.createElement("option");
      o.value = l.code;
      o.textContent = l.label;
      sel.appendChild(o);
    }
    sel.value = lang;
    sel.addEventListener("change", () => { setLang(sel.value); });
  });
}

// Switch language without a page reload: re-translate the static DOM, then let
// registered listeners refresh their runtime-built strings.
export async function setLang(code) {
  if (!isSupported(code) || code === lang) return false;
  try {
    await loadLocale(code);
  } catch (e) {
    console.error("i18n: failed to load locale " + code, e);
    return false;
  }
  lang = code;
  ready = true;
  try { localStorage.setItem(LS_KEY, code); } catch {}
  try { document.documentElement.lang = htmlLang(code); } catch {}
  applyI18n();
  syncLangSelectors();
  for (const fn of [...listeners]) {
    try { fn(code); } catch (e) { console.error("i18n listener failed", e); }
  }
  return true;
}
