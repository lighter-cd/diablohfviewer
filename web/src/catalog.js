// web/src/catalog.js
//
// Turn a sprite.list result into a nested family -> class -> type tree and
// render it as a clickable DOM tree. Clicking a leaf sprite calls onSelect(sprite).

// Build a nested map: family -> class -> type -> sprite[]
export function buildTree(sprites) {
  const tree = {};
  for (const s of sprites) {
    const f = s.family || "(unknown)";
    const c = s.class || "(unknown)";
    const t = s.type || "";
    (tree[f] = tree[f] || {});
    (tree[f][c] = tree[f][c] || {});
    (tree[f][c][t] = tree[f][c][t] || []).push(s);
  }
  return tree;
}

function leafLabel(sprite) {
  const base = sprite.name.split("\\").pop().split("/").pop();
  return (sprite.type ? sprite.type + " · " : "") + base;
}

// Render `tree` into `container` (a <ul>). onSelect(sprite) fires on leaf click.
export function renderCatalog(container, tree, onSelect) {
  container.innerHTML = "";
  const frag = document.createDocumentFragment();

  for (const fam of Object.keys(tree).sort()) {
    const famLi = document.createElement("li");
    famLi.className = "cat-family";
    famLi.textContent = fam;

    const classUl = document.createElement("ul");
    for (const cls of Object.keys(tree[fam]).sort()) {
      const clsLi = document.createElement("li");
      clsLi.className = "cat-class";
      clsLi.textContent = cls;

      const typeUl = document.createElement("ul");
      for (const type of Object.keys(tree[fam][cls]).sort()) {
        for (const sprite of tree[fam][cls][type]) {
          const spLi = document.createElement("li");
          spLi.className = "cat-sprite";
          spLi.textContent = leafLabel(sprite);
          spLi.title = sprite.name + (sprite.width ? `  (w=${sprite.width})` : "");
          spLi.addEventListener("click", () => {
            container.querySelectorAll(".cat-sprite.selected")
              .forEach((el) => el.classList.remove("selected"));
            spLi.classList.add("selected");
            onSelect(sprite);
          });
          typeUl.appendChild(spLi);
        }
      }
      clsLi.appendChild(typeUl);
      classUl.appendChild(clsLi);
    }
    famLi.appendChild(classUl);
    frag.appendChild(famLi);
  }
  container.appendChild(frag);
}
