// web/src/catalog.js
//
// Turn a sprite.list result into a nested tree, grouped either by sprite
// family/class/type (combined mode) or by the owning MPQ package (mpq mode),
// and render it as a collapsible, clickable DOM tree. Clicking a leaf sprite
// calls onSelect(sprite). Every branch starts collapsed by default.

import { tOpt } from "./i18n.js?v=20261113d";

const unknown = () => tOpt("common.unknown", "(unknown)");

// Chain of keys (root -> ... -> leaf) for one sprite in the chosen mode.
function keyPath(sprite, mode) {
  if (mode === "mpq") {
    // group by owning MPQ, then by the in-archive directory path.
    const archive = sprite.archive || unknown();
    const parts = (sprite.name || "")
      .split(/[\\/]/)
      .filter((p) => p.length > 0);
    return [archive, ...parts];
  }
  // combined / family mode: family -> class -> type -> basename
  const segs = [sprite.family || unknown(), sprite.class || unknown()];
  if (sprite.type) segs.push(sprite.type);
  segs.push(leafBasename(sprite.name));
  return segs;
}

function leafBasename(name) {
  return (name || "").split(/[\\/]/).pop();
}

// Build a nested node map from sprites. Each node: { label, children: Map, sprite }.
// Leaf nodes (the sprite itself) carry `sprite` and have no children.
export function buildTree(sprites, mode = "family") {
  const root = { label: "", children: new Map(), sprite: null };
  for (const s of sprites || []) {
    const keys = keyPath(s, mode);
    let node = root;
    for (const k of keys) {
      if (!node.children.has(k))
        node.children.set(k, { label: k, children: new Map(), sprite: null });
      node = node.children.get(k);
    }
    node.sprite = s; // leaf holds the actual sprite
  }
  return root;
}

function leafLabel(sprite) {
  return leafBasename(sprite.name);
}

// Recursively render a node's children into `ul`.
function renderChildren(ul, node, onSelect, container) {
  for (const child of node.children.values()) {
    const li = document.createElement("li");

    if (child.children.size > 0) {
      // branch node (collapsible)
      li.className = "cat-branch";
      const head = document.createElement("span");
      head.className = "cat-head collapsed";
      head.textContent = child.label;
      // Only the localized placeholder needs re-translating on a language
      // switch; real family/class/archive names are backend data.
      if (child.label === unknown()) head.setAttribute("data-i18n", "common.unknown");
      const sub = document.createElement("ul");
      sub.className = "cat-sub";
      sub.hidden = true; // default collapsed
      renderChildren(sub, child, onSelect, container);

      head.addEventListener("click", () => {
        const collapsed = sub.hidden;
        sub.hidden = !collapsed;
        head.classList.toggle("collapsed", !collapsed);
        head.classList.toggle("expanded", collapsed);
      });
      li.appendChild(head);
      li.appendChild(sub);
    } else {
      // leaf sprite
      li.className = "cat-sprite";
      li.textContent = leafLabel(child.sprite);
      li.title = child.sprite.name +
        (child.sprite.width ? `  (w=${child.sprite.width})` : "");
      li.addEventListener("click", (ev) => {
        ev.stopPropagation();
        container.querySelectorAll(".cat-sprite.selected")
          .forEach((el) => el.classList.remove("selected"));
        li.classList.add("selected");
        onSelect(child.sprite);
      });
    }
    ul.appendChild(li);
  }
}

// Render the tree (root node map) into `container`.
export function renderCatalog(container, root, onSelect) {
  container.innerHTML = "";
  const ul = document.createElement("ul");
  ul.className = "cat-root";
  renderChildren(ul, root, onSelect, container);
  container.appendChild(ul);
  return ul;
}

// Expand or collapse every branch in `container`.
export function setAllExpanded(container, expanded) {
  container.querySelectorAll(".cat-sub").forEach((ul) => { ul.hidden = !expanded; });
  container.querySelectorAll(".cat-head").forEach((h) => {
    h.classList.toggle("collapsed", !expanded);
    h.classList.toggle("expanded", expanded);
  });
}
