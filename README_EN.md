> **Repository summary:** Diablo 1 / Hellfire MPQ research & sprite·level·UI viewer — a pure C/C++ native mcp-server (stdio for AI Agents, WebSocket for the browser front-end). You must supply your own legitimate Hellfire files; this project provides no game assets.

# DiabloHFConverter

> *Diablo: Hellfire* is the most ambitious expansion of the original 1997 *Diablo*. Beyond the original cathedral, catacombs and hell, it adds the hidden Crypt and Nest levels, introduces the one-of-a-kind Monk class, and leaves behind a whole treasure trove of sprites, levels and UI art now buried by time. This forgotten treasure deserves to be studied and appreciated with care.

This is an open-source toolkit for **Diablo 1 / Hellfire researchers, tool authors, and AI Agents**.

The motivation behind this project is to **solve the long-standing fragmentation of Hellfire's data and tools** — sprite, palette, level, scene, and UI-layout research materials are scattered everywhere, and the corresponding viewers and decoders each stand alone. DiabloHFConverter **brings all these materials and tools together into a single, unified Web interface**: the browser front-end (`web/`) calls the very same local C/C++ decoding engine through a WebSocket bridge, so researchers no longer have to jump between scattered utilities — they can browse, compare, and explore all of Hellfire's art and level assets from one page.

## What this is

DiabloHFConverter is a **pure C/C++ native `mcp-server`** (no Node.js runtime dependency) for **reading, decoding and studying the MPQ archives of Diablo 1 / Hellfire**. It exposes its capabilities through two transports:

- **stdio transport**: exposes standard JSON-RPC 2.0 tools to AI Agents (Claude Desktop / CLI tools), letting models "see" the game assets directly;
- **WebSocket bridge**: exposes the same set of tools to the browser front-end (`web/`, native ES Module + Canvas, zero build) for visualizing sprites, level maps, full scenes and UI layouts.

All MPQ reading, sprite decoding, level rendering, scene reconstruction and A\* pathfinding run in the local C/C++ server; the front-end only paints the decoded RGBA frames.

What it can do includes: list / extract / verify / recover files inside MPQs; decode `.clx` / `.cel` / `.cl2` / `.pcx` sprites and palettes; reconstruct the full scenes of Tristram town and the L1–L4 preset dungeons with lighting and pathfinding; natively regenerate the Cathedral's procedural dungeons from a seed; restore the DiabloUI screens' layouts; and organize a full per-frame visual catalog of player skills / monster attacks / traps.

## What this is NOT

- **It is not a game** — it cannot run, launch, or let you play Diablo / Hellfire;
- **It is not a reverse-engineered remake like DevilutionX** — it does not reimplement the game engine so you can play the original;
- **It is not a mod, cheat, or trainer** — it does not modify any game data, save files, or running process;
- **It does not bundle, package, or distribute any game asset files** (MPQs, palettes, sprites, audio, etc.).

## ⚠️ Important notice about game assets

**This project solemnly declares: users must own their own official, legitimate copy of *Diablo: Hellfire* (including `DIABDAT.MPQ`, etc.). This project does not provide, host, link to, or generate any copyrighted game assets.**

All decoding, viewing, and reconstruction features depend on the legitimate MPQ archives you already have locally. Without legitimate game files, any feature related to game content will not run. Please obtain your own copy through legal channels.

---

> For build, run, and test instructions, see the Chinese [`README.md`](README.md).

## License

This project's code is released under the **MIT License** (full text in [`LICENSE`](LICENSE));
copyright holder: **lighter_cd** `<lighter_cd@hotmail.com>`.

In short: **anyone may use this code freely and for anything** — use, copy, modify, merge, publish, distribute, sublicense, and even sell it, with no prior permission and no fee. The only obligation is to keep the original copyright notice and this permission notice (ship a copy of `LICENSE` with the source or binary). The software is provided "as is", without warranty of any kind.

Bundled dependencies keep their own licenses, both compatible with MIT:

| Dependency | License |
|------------|---------|
| `3rdparty/StormLib/` (git submodule) | MIT (Copyright (c) 1999-2013 Ladislav Zezula) |
| `3rdparty/nlohmann/json.hpp` | MIT (Copyright (c) 2013-2022 Niels Lohmann) |

> ⚠️ **MIT covers this project's code only.** The Diablo 1 / Hellfire game assets (MPQs, sprites, palettes, audio, …) are *not* covered by this license and remain the property of their rights holders. This project does not provide, host, link to, or distribute any game assets; you must own your own legitimate copy (see the notice above and [`docs/COMPLIANCE.md`](docs/COMPLIANCE.md)).

> **About commit emails**: `*@desktop_pc.com` addresses in the git history are the same author's
> per-machine commit identities; copyright and contact details are as stated in `LICENSE`. The
> repository's [`.mailmap`](.mailmap) folds them into `lighter_cd <lighter_cd@hotmail.com>` for local
> `git log` / `git shortlog` display only — history is not rewritten.

## Release (maintainers)

Versioning is `vMAJOR.MINOR.PATCH` (e.g. `v0.1.0`). `scripts/release.py` (Python stdlib only,
no `gh` CLI needed) builds a portable zip, tags the commit and publishes the archive as a binary
asset on **both** remotes — found by URL host (`gitee.com` / `github.com`), not by remote name.

```bash
python scripts/release.py --version v0.1.0 --dry-run   # package + checksum, no network
$env:GITHUB_TOKEN="ghp_xxx"; $env:GITEE_TOKEN="xxx"    # then the real run
python scripts/release.py --version v0.1.0
```

Artifacts land in `dist/` (`DiabloHFViewer-<ver>-win64.zip` + `.sha256`). The zip contains
`mcp-server.exe`, the MinGW `libwinpthread-1.dll`, `web/`, optional
`mpq-info/dict/recovered.json`, `start.bat`, `LICENSE`, `README.md` and `VERSION.txt` — and the
script refuses to package any `*.mpq`. Test: `python tests/release_pack_test.py`.

> Day-to-day pushes go to Gitee only (`git push` → remote `origin`). GitHub is a mirror named
> `github` and is updated only on explicit request (`git push github master`); a release is an
> explicit action, so the script updates both platforms' tags and Releases.

## Front-end i18n

The viewer UI is bilingual: use the **Language / 语言** dropdown in the top-right of `index.html`
or `level.html` to switch between **English (`en`)** and **简体中文 (`zh-cn`)** instantly (no reload).
The choice is remembered in `localStorage`. All display strings live in plain JSON config files under
`web/i18n/` (`locales.json` + one `<code>.json` per language); adding a language needs no code change —
drop in a new JSON file and add one entry to `locales.json`. Validate the tables with
`node tests/i18n_config_test.mjs`.
