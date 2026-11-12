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
